#include "html_doc.h"

#include "url_util.h"
#include "wav_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iterator>
#include <initializer_list>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

/**
 * @brief Lowercases every character of a string (ASCII-aware via unsigned char cast).
 * @param s String to lowercase, taken by value and modified in place.
 * @return The lowercased string.
 */
std::string ToLower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/**
 * @brief Checks whether a tag is a void element (no closing tag, e.g. <br>, <img>).
 * @param tag Lowercase tag name to check.
 * @return True if `tag` is one of the recognized void elements.
 */
bool IsVoidTag(const std::string &tag) {
    static const std::unordered_set<std::string> kVoid = {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr",
    };
    return kVoid.count(tag) != 0;
}

// Content of these is taken verbatim (no nested-tag parsing) up to the
// matching close tag -- real HTML rule for <script>/<style>/<textarea>,
// each for a different reason (script/style bodies routinely contain '<'
// that isn't markup; textarea's is meant to preserve exactly what's typed).
/**
 * @brief Checks whether a tag's content should be taken verbatim (no nested-tag parsing) up to its matching close tag.
 * @param tag Lowercase tag name to check.
 * @return True for script/style/textarea.
 */
bool IsRawTextTag(const std::string &tag) { return tag == "script" || tag == "style" || tag == "textarea"; }

/**
 * @brief Finds the index of the next case-insensitive "</tag" occurrence at-or-after `from`.
 * @param html Source HTML text to search.
 * @param tag Lowercase tag name whose close tag is sought.
 * @param from Index to start searching from.
 * @return Index of the matching "</tag" occurrence, or std::string::npos if none is found.
 */
size_t FindCloseTagCI(const std::string &html, const std::string &tag, size_t from) {
    std::string needle = "</" + tag;
    std::string lower_html = ToLower(html.substr(from));
    size_t pos = lower_html.find(needle);
    return pos == std::string::npos ? std::string::npos : from + pos;
}

// &name; and &#NN;/&#xHH; -- the common subset real-world pages actually
// use, not the full HTML5 named-character-reference table (over 2000
// entries, almost all obscure symbols this monospace-only renderer has no
// glyph for anyway -- see IconForFilename/g_icon_font's own ASCII-first
// precedent, editor.h, for the same "font coverage" reasoning).
/**
 * @brief Decodes HTML character references (&name; and &#NN;/&#xHH;) in a string, covering a common subset (not the full HTML5 named-entity table).
 * @param s Text to decode.
 * @return `s` with recognized entities replaced by their literal characters; unrecognized ones are left as-is.
 */
std::string DecodeEntities(const std::string &s) {
    static const std::unordered_map<std::string, std::string> kNamed = {
        {"amp", "&"},     {"lt", "<"},        {"gt", ">"},     {"quot", "\""}, {"apos", "'"},
        {"nbsp", " "},    {"copy", "(c)"},    {"reg", "(R)"},  {"mdash", "--"}, {"ndash", "-"},
        {"hellip", "..."},{"rsquo", "'"},     {"lsquo", "'"},  {"rdquo", "\""}, {"ldquo", "\""},
        {"trade", "(TM)"},{"deg", " deg"},
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) {
            out += s[i++];
            continue;
        }
        std::string body = s.substr(i + 1, semi - i - 1);
        if (!body.empty() && body[0] == '#') {
            bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
            const char *num_start = body.c_str() + (hex ? 2 : 1);
            char *end = nullptr;
            long cp = std::strtol(num_start, &end, hex ? 16 : 10);
            // Any valid scalar value, encoded as UTF-8 -- the text fonts cover
            // far more than ASCII (a literal U+2014 em-dash in page text
            // already renders), so "&#8212;" must not survive as source text.
            // A codepoint the atlas lacks draws as nothing, which still beats
            // showing markup. Surrogates/0/out-of-range fall through as-is.
            if (end != num_start && cp > 0 && cp < 0x110000 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
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
                i = semi + 1;
                continue;
            }
        }
        auto it = kNamed.find(body);
        if (it != kNamed.end()) {
            out += it->second;
            i = semi + 1;
            continue;
        }
        out += s[i++];  // unrecognized -- keep the literal '&', try again from the next char
    }
    return out;
}

struct TagParseResult {
    std::string tag;
    std::unordered_map<std::string, std::string> attrs;
    bool closing = false;
    bool self_closing = false;
};

// Parses one tag starting at `pos` (the character right after '<'; `<` of
// a closing tag has already been confirmed present by the caller via
// html[pos]=='/'). Returns the index just past the tag's own '>'.
/**
 * @brief Parses a single start or end tag (name, closing/self-closing flags, and attributes) starting right after its '<'.
 * @param html Source HTML text being parsed.
 * @param pos Index of the character right after the tag's opening '<'.
 * @param out Result struct populated with the parsed tag name, attributes, and closing/self-closing flags.
 * @return Index just past the tag's own '>'.
 */
size_t ParseTag(const std::string &html, size_t pos, TagParseResult &out) {
    size_t n = html.size();
    size_t i = pos;
    if (i < n && html[i] == '/') {
        out.closing = true;
        i++;
    }
    size_t name_start = i;
    while (i < n && (std::isalnum(static_cast<unsigned char>(html[i])) || html[i] == '-' || html[i] == ':')) i++;
    out.tag = ToLower(html.substr(name_start, i - name_start));

    while (i < n && html[i] != '>') {
        while (i < n && std::isspace(static_cast<unsigned char>(html[i]))) i++;
        if (i < n && html[i] == '/') {
            out.self_closing = true;
            i++;
            continue;
        }
        if (i >= n || html[i] == '>') break;
        size_t attr_name_start = i;
        while (i < n && html[i] != '=' && html[i] != '>' && !std::isspace(static_cast<unsigned char>(html[i])) &&
               html[i] != '/') {
            i++;
        }
        std::string attr_name = ToLower(html.substr(attr_name_start, i - attr_name_start));
        if (attr_name.empty()) {
            i++;  // stray char (e.g. a bare '"'); skip rather than loop forever
            continue;
        }
        while (i < n && std::isspace(static_cast<unsigned char>(html[i]))) i++;
        std::string value;
        if (i < n && html[i] == '=') {
            i++;
            while (i < n && std::isspace(static_cast<unsigned char>(html[i]))) i++;
            if (i < n && (html[i] == '"' || html[i] == '\'')) {
                char quote = html[i++];
                size_t val_start = i;
                while (i < n && html[i] != quote) i++;
                value = html.substr(val_start, i - val_start);
                if (i < n) i++;  // consume closing quote
            } else {
                size_t val_start = i;
                while (i < n && !std::isspace(static_cast<unsigned char>(html[i])) && html[i] != '>') i++;
                value = html.substr(val_start, i - val_start);
            }
        }
        out.attrs[attr_name] = DecodeEntities(value);
    }
    if (i < n && html[i] == '>') i++;
    return i;
}

// Subtrees whose text isn't prose to be scanned for math -- mirrors
// MathJax's own default skip-tag list (script/style/pre/code/textarea),
// plus "math" itself so a span already extracted is never rescanned.
/**
 * @brief Checks whether a tag's text content should be skipped when scanning for math spans (mirrors MathJax's default skip-tag list, plus "math" itself).
 * @param tag Lowercase tag name to check.
 * @return True for script/style/pre/code/textarea/math.
 */
bool IsMathSkipTag(const std::string &tag) {
    return tag == "script" || tag == "style" || tag == "pre" || tag == "code" || tag == "textarea" || tag == "math";
}

// Finds the next \(..\), \[..\], $$..$$, or $..$ span at-or-after `from`.
// On a match, [content_start,content_end) bounds the raw LaTeX (delimiters
// excluded), `span_end` is the index just past the closing delimiter, and
// `display` is true for \[..\]/$$..$$. Returns std::string::npos if none
// found before the end of `s`. The single-`$` form only matches when its
// content has no adjacent whitespace and contains no blank line, the same
// conservative heuristic real MathJax configs use to avoid swallowing an
// ordinary "$5 and $10" sentence as math.
/**
 * @brief Finds the next \(..\), \[..\], $$..$$, or $..$ math span at-or-after `from`, applying a conservative heuristic for single-`$` spans to avoid false positives like "$5 and $10".
 * @param s Text to search.
 * @param from Index to start searching from.
 * @param content_start Set to the index where the raw LaTeX content begins (delimiter excluded).
 * @param content_end Set to the index where the raw LaTeX content ends (delimiter excluded).
 * @param span_end Set to the index just past the closing delimiter.
 * @param display Set to true if the span is a display-style delimiter (\[..\] or $$..$$).
 * @return Index of the opening delimiter, or std::string::npos if no span was found before the end of `s`.
 */
size_t FindNextMathSpan(const std::string &s, size_t from, size_t &content_start, size_t &content_end,
                         size_t &span_end, bool &display) {
    for (size_t i = from; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '[')) {
            bool disp = s[i + 1] == '[';
            std::string close = disp ? "\\]" : "\\)";
            size_t end = s.find(close, i + 2);
            if (end == std::string::npos) continue;
            content_start = i + 2;
            content_end = end;
            span_end = end + 2;
            display = disp;
            return i;
        }
        if (s[i] == '$') {
            bool disp = i + 1 < s.size() && s[i + 1] == '$';
            size_t open_len = disp ? 2 : 1;
            size_t body_start = i + open_len;
            if (body_start >= s.size()) continue;
            if (!disp && std::isspace(static_cast<unsigned char>(s[body_start]))) continue;  // "$ 5" -- not math
            std::string close = disp ? "$$" : "$";
            size_t search_from = body_start;
            size_t end = std::string::npos;
            while (search_from < s.size()) {
                size_t cand = s.find(close, search_from);
                if (cand == std::string::npos) break;
                if (cand == body_start) {
                    search_from = cand + open_len;  // empty span -- not math, keep looking
                    continue;
                }
                if (!disp && std::isspace(static_cast<unsigned char>(s[cand - 1]))) {
                    search_from = cand + open_len;  // "...text $" -- trailing space before close, not math
                    continue;
                }
                if (s.find('\n', body_start) < cand) break;  // blank-line-spanning $..$ almost never real math
                end = cand;
                break;
            }
            if (end == std::string::npos) continue;
            content_start = body_start;
            content_end = end;
            span_end = end + open_len;
            display = disp;
            return i;
        }
    }
    return std::string::npos;
}

// Splits math spans out of `parent`'s Text children into sibling <math>
// elements (attrs["display"]="1"/"0"), recursing into element children
// that aren't in IsMathSkipTag's list. Each <math> node's raw LaTeX source
// is stashed as its own single Text child, mirroring how <style>'s raw CSS
// text is stored (above) -- main.cpp's own mini LaTeX layout (js_engine.h's
// neighbor for math, not this raylib-free file, since typesetting needs
// real font metrics) reads it back out via HtmlCollectRawText.
/**
 * @brief Splits math spans out of `parent`'s Text children into sibling <math> elements holding the raw LaTeX source, recursing into element children not in IsMathSkipTag's list.
 * @param parent Node whose children are rewritten in place with extracted <math> siblings interleaved.
 */
void ExtractMathFromChildren(DomNode *parent) {
    std::vector<std::unique_ptr<DomNode>> out_children;
    for (auto &child : parent->children) {
        if (child->type == DomNodeType::Element) {
            if (!IsMathSkipTag(child->tag)) ExtractMathFromChildren(child.get());
            out_children.push_back(std::move(child));
            continue;
        }
        const std::string text = child->text;  // copy: `child` may be moved-from below before every use of it ends
        size_t pos = 0;
        bool any = false;
        while (pos < text.size()) {
            size_t content_start, content_end, span_end;
            bool display;
            size_t open = FindNextMathSpan(text, pos, content_start, content_end, span_end, display);
            if (open == std::string::npos) break;
            any = true;
            if (open > pos) {
                auto t = std::make_unique<DomNode>();
                t->type = DomNodeType::Text;
                t->text = text.substr(pos, open - pos);
                t->parent = parent;
                out_children.push_back(std::move(t));
            }
            auto math = std::make_unique<DomNode>();
            math->type = DomNodeType::Element;
            math->tag = "math";
            math->attrs["display"] = display ? "1" : "0";
            math->parent = parent;
            auto latex = std::make_unique<DomNode>();
            latex->type = DomNodeType::Text;
            latex->text = text.substr(content_start, content_end - content_start);
            latex->parent = math.get();
            math->children.push_back(std::move(latex));
            out_children.push_back(std::move(math));
            pos = span_end;
        }
        if (!any) {
            out_children.push_back(std::move(child));
            continue;
        }
        if (pos < text.size()) {
            auto t = std::make_unique<DomNode>();
            t->type = DomNodeType::Text;
            t->text = text.substr(pos);
            t->parent = parent;
            out_children.push_back(std::move(t));
        }
    }
    parent->children = std::move(out_children);
}

/**
 * @brief Extracts math spans from the whole document's node tree, if it has a root.
 * @param doc Document whose tree is walked and rewritten in place.
 */
void ExtractMathSpans(HtmlDoc &doc) {
    if (doc.root) ExtractMathFromChildren(doc.root.get());
}

}  // namespace

const std::string &DomNode::Id() const {
    static const std::string kEmpty;
    auto it = attrs.find("id");
    return it == attrs.end() ? kEmpty : it->second;
}

const std::string &DomNode::Class() const {
    static const std::string kEmpty;
    auto it = attrs.find("class");
    return it == attrs.end() ? kEmpty : it->second;
}

bool IsHtmlPath(const std::string &path) {
    std::string lower = ToLower(path);
    return lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".html") == 0
               ? true
               : (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".htm") == 0);
}

namespace {
std::string ScriptTypeOf(const std::unordered_map<std::string, std::string> &attrs) {
    auto type = attrs.find("type");
    std::string text = type == attrs.end() ? "" : type->second;
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t semicolon = text.find(';');
    if (semicolon != std::string::npos) text.resize(semicolon);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}
// A <script> whose type names JavaScript (or nothing at all) runs; any other type is a data block.
bool IsExecutableScriptType(const std::unordered_map<std::string, std::string> &attrs) {
    const std::string type = ScriptTypeOf(attrs);
    return type.empty() || type == "module" || type == "text/javascript" || type == "application/javascript" ||
           type == "text/ecmascript" || type == "application/ecmascript";
}
bool IsModuleScriptType(const std::unordered_map<std::string, std::string> &attrs) { return ScriptTypeOf(attrs) == "module"; }
}  // namespace


void ParseHtml(const std::string &html, HtmlDoc &out, bool full_document, bool compute_styles) {
    out.root = std::make_unique<DomNode>();
    out.root->type = DomNodeType::Element;
    out.root->tag = "#document";
    out.title.clear();
    out.scripts.clear();
    out.script_info.clear();
    out.detached_nodes.clear();

    std::vector<DomNode *> stack;
    stack.push_back(out.root.get());
    std::string text_buf;

    /**
     * @brief Flushes any buffered raw text into a new decoded Text node appended to the innermost open element, then clears the buffer.
     */
    auto flush_text = [&]() {
        if (text_buf.empty()) return;
        auto node = std::make_unique<DomNode>();
        node->type = DomNodeType::Text;
        node->text = DecodeEntities(text_buf);
        node->parent = stack.back();
        stack.back()->children.push_back(std::move(node));
        text_buf.clear();
    };

    size_t i = 0, n = html.size();
    while (i < n) {
        if (html[i] != '<') {
            text_buf += html[i++];
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i + 4);
            i = (end == std::string::npos) ? n : end + 3;
            continue;
        }
        if (i + 1 < n && html[i + 1] == '!') {  // <!DOCTYPE ...>
            size_t end = html.find('>', i);
            i = (end == std::string::npos) ? n : end + 1;
            continue;
        }
        bool looks_like_tag = i + 1 < n && (html[i + 1] == '/' || std::isalpha(static_cast<unsigned char>(html[i + 1])));
        if (!looks_like_tag) {
            text_buf += html[i++];  // a bare '<' in text (not valid HTML, but common in the wild) -- keep it literal
            continue;
        }
        flush_text();
        TagParseResult tr;
        i = ParseTag(html, i + 1, tr);
        if (tr.closing) {
            for (size_t k = stack.size(); k-- > 1;) {
                if (stack[k]->tag == tr.tag) {
                    stack.resize(k);
                    break;
                }
            }
            continue;
        }

        auto node = std::make_unique<DomNode>();
        node->type = DomNodeType::Element;
        node->tag = tr.tag;
        node->attrs = std::move(tr.attrs);
        if (auto value = node->attrs.find("value"); value != node->attrs.end()) node->form_value = value->second;
        node->form_checked = node->attrs.count("checked") != 0;
        node->form_disabled = node->attrs.count("disabled") != 0;
        node->details_open = node->attrs.count("open") != 0;
        if (node->tag == "canvas") {
            auto dimension = [&node](const char *name, int fallback) {
                auto it = node->attrs.find(name);
                if (it == node->attrs.end()) return fallback;
                char *end = nullptr;
                long value = std::strtol(it->second.c_str(), &end, 10);
                return end == it->second.c_str() ? fallback : static_cast<int>(std::max(1L, std::min(value, 8192L)));
            };
            node->canvas_width = dimension("width", 300);
            node->canvas_height = dimension("height", 150);
        }
        node->parent = stack.back();
        DomNode *raw = node.get();
        stack.back()->children.push_back(std::move(node));

        if (IsRawTextTag(tr.tag)) {
            size_t close_start = FindCloseTagCI(html, tr.tag, i);
            std::string raw_text = (close_start == std::string::npos) ? html.substr(i) : html.substr(i, close_start - i);
            if (tr.tag == "script" && IsExecutableScriptType(raw->attrs)) {
                out.scripts.push_back(raw_text);
                out.script_info.push_back({IsModuleScriptType(raw->attrs), ""});
                // Keep the source on the element as well as in the legacy
                // execution list.  Session-level resource loading rebuilds
                // that list in DOM order once local `src` files are folded
                // in, so inline and external scripts interleave correctly.
                auto tnode = std::make_unique<DomNode>();
                tnode->type = DomNodeType::Text;
                tnode->text = raw_text;
                tnode->parent = raw;
                raw->children.push_back(std::move(tnode));
            } else if (tr.tag == "script") {
                // A data block (type="application/json", a template, ...):
                // never executed, but its text stays readable from the DOM.
                auto tnode = std::make_unique<DomNode>();
                tnode->type = DomNodeType::Text;
                tnode->text = raw_text;
                tnode->parent = raw;
                raw->children.push_back(std::move(tnode));
            } else if (tr.tag == "style") {
                auto tnode = std::make_unique<DomNode>();
                tnode->type = DomNodeType::Text;
                tnode->text = raw_text;
                tnode->parent = raw;
                raw->children.push_back(std::move(tnode));
            }
            if (tr.tag == "textarea") raw->form_value = DecodeEntities(raw_text);
            if (close_start == std::string::npos) {
                i = n;
            } else {
                size_t gt = html.find('>', close_start);
                i = (gt == std::string::npos) ? n : gt + 1;
            }
            continue;
        }

        if (!tr.self_closing && !IsVoidTag(tr.tag)) stack.push_back(raw);
    }
    flush_text();

    // Browsers always give a page <html> and <body> even when the markup
    // omits them (fragment parses skip this -- their nodes graft into an
    // existing tree): document.body must be an element (pages hang state off it --
    // the `body.nav-open` off-canvas drawer idiom), and body-/`:root`-keyed
    // selectors need something to match. Synthesize the missing wrappers
    // around the parsed content; a page that wrote its own keeps exactly the
    // tree it wrote. Neither tag carries UA styles here, so a fragment's
    // rendering is unchanged by the extra layers.
    auto find_tag = [](DomNode *n, const std::string &tag) -> DomNode * {
        std::function<DomNode *(DomNode *)> walk = [&](DomNode *cur) -> DomNode * {
            if (cur->type == DomNodeType::Element && cur->tag == tag) return cur;
            for (auto &c : cur->children)
                if (DomNode *r = walk(c.get())) return r;
            return nullptr;
        };
        return walk(n);
    };
    DomNode *html_el = full_document ? find_tag(out.root.get(), "html") : nullptr;
    if (full_document && !html_el) {
        auto html_node = std::make_unique<DomNode>();
        html_node->type = DomNodeType::Element;
        html_node->tag = "html";
        html_node->parent = out.root.get();
        html_el = html_node.get();
        for (auto &c : out.root->children) {
            c->parent = html_el;
            html_el->children.push_back(std::move(c));
        }
        out.root->children.clear();
        out.root->children.push_back(std::move(html_node));
    }
    if (full_document && !find_tag(out.root.get(), "body")) {
        auto body_node = std::make_unique<DomNode>();
        body_node->type = DomNodeType::Element;
        body_node->tag = "body";
        body_node->parent = html_el;
        DomNode *body_el = body_node.get();
        std::vector<std::unique_ptr<DomNode>> kept;  // <head> stays a sibling of <body>, not a child
        for (auto &c : html_el->children) {
            if (c->type == DomNodeType::Element && c->tag == "head") {
                kept.push_back(std::move(c));
                continue;
            }
            c->parent = body_el;
            body_el->children.push_back(std::move(c));
        }
        html_el->children = std::move(kept);
        html_el->children.push_back(std::move(body_node));
    }

    // <title> is always near the document's start in practice, so a plain
    // breadth-first search (rather than a depth-first walk that might
    // detour deep into <body> first) finds it in the fewest node visits.
    // Walked by index, never by erasing the front: a page with no <title>
    // (every exported fragment) visits every node, and erase(begin())
    // shifted the whole queue each time -- O(nodes^2), a third of a large
    // LaTeX export (plans/MEPML_PERFORMANCE_PLAN.md).
    std::vector<DomNode *> queue = {out.root.get()};
    for (size_t qi = 0; qi < queue.size() && out.title.empty(); qi++) {
        DomNode *cur = queue[qi];
        if (cur->type == DomNodeType::Element && cur->tag == "title") {
            for (const auto &c : cur->children) {
                if (c->type == DomNodeType::Text) out.title += c->text;
            }
            break;
        }
        for (auto &c : cur->children) queue.push_back(c.get());
    }

    ExtractMathSpans(out);
    if (compute_styles) ComputeStyles(out);
}

namespace {
HtmlUrlFetcher &UrlFetcherSlot() {
    static HtmlUrlFetcher fetcher;
    return fetcher;
}

// Shared by the local and network loaders: folds every stylesheet
// `read_resource` can produce into the DOM as a <style> node, then
// rebuilds doc.scripts in document order (external bodies via
// `read_resource`, inline ones from their text children).
void FoldLinkedResources(HtmlDoc &doc, const std::function<bool(const std::string &, std::string &)> &read_resource);
}  // namespace

void SetHtmlUrlFetcher(HtmlUrlFetcher fetcher) { UrlFetcherSlot() = std::move(fetcher); }
const HtmlUrlFetcher &GetHtmlUrlFetcher() { return UrlFetcherSlot(); }

void LoadRemoteHtmlResources(HtmlDoc &doc, const std::string &base_url) {
    doc.resource_base_url = base_url;
    FoldLinkedResources(doc, [&](const std::string &href, std::string &contents) {
        const HtmlUrlFetcher &fetch = GetHtmlUrlFetcher();
        if (href.empty() || !fetch) return false;
        HtmlFetchResult result = fetch(urlutil::ResolveUrl(base_url, href));
        if (result.status != 200) return false;
        contents = std::move(result.body);
        return true;
    });
}

void LoadLocalHtmlResources(HtmlDoc &doc, const std::string &base_dir) {
    doc.resource_base_dir = base_dir;
    const std::filesystem::path base(base_dir);
    FoldLinkedResources(doc, [&](const std::string &href, std::string &contents) {
        if (href.empty() || href.find("://") != std::string::npos) return false;
        std::filesystem::path resolved(href);
        if (resolved.is_relative()) resolved = base / resolved;
        std::ifstream input(resolved, std::ios::binary);
        if (!input) return false;
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        return true;
    });
}

namespace {
void FoldLinkedResources(HtmlDoc &doc, const std::function<bool(const std::string &, std::string &)> &read_resource) {
    std::function<void(DomNode *)> load_styles = [&](DomNode *node) {
        if (!node) return;
        if (node->type == DomNodeType::Element && node->tag == "link") {
            auto rel = node->attrs.find("rel"), href = node->attrs.find("href");
            if (rel != node->attrs.end() && href != node->attrs.end() && ToLower(rel->second) == "stylesheet") {
                std::string css;
                if (read_resource(href->second, css)) {
                    node->tag = "style";
                    auto text = std::make_unique<DomNode>(); text->type = DomNodeType::Text; text->text = std::move(css); text->parent = node;
                    node->children.push_back(std::move(text));
                }
            }
        }
        for (const auto &child : node->children) load_styles(child.get());
    };
    load_styles(doc.root.get());
    doc.scripts.clear();
    doc.script_info.clear();
    std::function<void(const DomNode *)> collect_scripts = [&](const DomNode *node) {
        if (!node) return;
        if (node->type == DomNodeType::Element && node->tag == "script" && IsExecutableScriptType(node->attrs)) {
            std::string code;
            HtmlDoc::ScriptInfo info;
            info.is_module = IsModuleScriptType(node->attrs);
            auto src = node->attrs.find("src");
            if (src != node->attrs.end()) {
                read_resource(src->second, code);
                if (!doc.resource_base_url.empty()) info.url = urlutil::ResolveUrl(doc.resource_base_url, src->second);
                else if (!doc.resource_base_dir.empty()) info.url = "file://" + (std::filesystem::path(doc.resource_base_dir) / src->second).lexically_normal().string();
            }
            else for (const auto &child : node->children) if (child->type == DomNodeType::Text) code += child->text;
            if (!code.empty()) { doc.scripts.push_back(std::move(code)); doc.script_info.push_back(std::move(info)); }
        }
        for (const auto &child : node->children) collect_scripts(child.get());
    };
    collect_scripts(doc.root.get());
    ComputeStyles(doc);
}
}  // namespace

namespace {

/**
 * @brief Builds the user-agent default ComputedStyle for a tag (block/inline, bold/italic/underline, heading font scale, list/display-none handling, etc.), before any CSS rules or inline styles are applied.
 * @param tag Lowercase tag name to get defaults for.
 * @return A ComputedStyle populated with this tag's UA defaults.
 */
ComputedStyle TagDefaults(const std::string &tag) {
    ComputedStyle s;
    s.block = true;
    if (tag == "p" || tag == "blockquote") {
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        s.bold = true;
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
        static const float kScales[6] = {2.0f, 1.5f, 1.17f, 1.05f, 0.9f, 0.8f};
        s.font_scale = kScales[tag[1] - '1'];
    } else if (tag == "a") {
        s.block = false;
        s.has_color = true;
        s.color_r = 90;
        s.color_g = 150;
        s.color_b = 230;
        s.underline = true;
    } else if (tag == "b" || tag == "strong") {
        s.block = false;
        s.bold = true;
    } else if (tag == "i" || tag == "em") {
        s.block = false;
        s.italic = true;
    } else if (tag == "u") {
        s.block = false;
        s.underline = true;
    } else if (tag == "s" || tag == "strike" || tag == "del") {
        s.block = false;
        s.strikethrough = true;
    } else if (tag == "code" || tag == "tt" || tag == "kbd" || tag == "samp") {
        s.block = false;
        s.monospace = true;
        s.font_family = HtmlFontFamily::Mono;
    } else if (tag == "pre") {
        s.monospace = true;
        s.font_family = HtmlFontFamily::Mono;
        s.preserve_whitespace = true;
        s.white_space = HtmlWhiteSpace::Pre;
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag == "ul" || tag == "ol") {
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
        // UA default list indent, expressed as the list's own left padding the
        // way real browsers do (`padding-inline-start`) rather than a
        // hardcoded per-depth step in the layout pass -- so it accumulates
        // linearly through nesting AND a page that sets `padding:0`/`margin:0`
        // on its lists (the standard nav-menu reset) gets no indent at all.
        // main.cpp's HtmlLayoutBlock adds no list indent of its own; the
        // cascade here is the single source of truth. 24px matches the step
        // main.cpp used before.
        s.padding.left.set = true;
        s.padding.left.value = 24.0f;
        s.padding.left.unit = CssLength::Unit::Px;
    } else if (tag == "hr") {
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag == "head" || tag == "style" || tag == "script" || tag == "title" || tag == "meta" ||
               tag == "link" || tag == "#comment") {
        s.display_none = true;
    } else if (tag == "mark") {
        // Highlighted match text (search results) -- inline, browser-default
        // yellow-on-dark-text look softened to just a color accent.
        s.block = false;
        s.has_color = true;
        s.color_r = 230;
        s.color_g = 170;
        s.color_b = 60;
        s.bold = true;
    } else if (tag == "span" || tag == "small" || tag == "label" || tag == "td" || tag == "th" || tag == "br" ||
               tag == "img" || tag == "audio" || tag == "video" || tag == "input" || tag == "button" || tag == "select" || tag == "textarea" || tag == "option" ||
               tag == "sub" || tag == "sup" || tag == "abbr" || tag == "cite" || tag == "q" || tag == "time" ||
               tag == "var" || tag == "dfn" || tag == "ins" || tag == "wbr" || tag == "bdi" || tag == "output") {
        s.block = false;
    } else if (tag == "math") {
        // Synthetic tag ExtractMathSpans (below) inserts for a \(..\)/\[..\]/
        // $..$/$$..$$ span -- inline vs display is per-node (its "display"
        // attr), not a property of the tag itself, so WalkAndStyle overrides
        // `block`/margins right after this call rather than branching here.
        s.block = false;
    }
    // <li> stays block (the default set above) -- it needs its own line
    // and left margin for HtmlLayoutBlock's (main.cpp) marker/indent logic
    // to ever run at all, unlike the genuinely-inline tags just above.
    // Everything else (div, section, article, header, footer, main, nav,
    // table/tr/thead/tbody, and any tag this parser has never heard of)
    // falls through as a plain block container with no extra styling --
    // still shown, just with no special treatment, matching this file's
    // own "unrecognized tag renders as a generic container" tolerance.
    return s;
}

struct CssSimpleSelector {
    struct Attribute { std::string name, value; char op = 0; };
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::vector<Attribute> attributes;
    std::vector<std::pair<std::string, std::string>> pseudos;
};

struct ParsedSelector {
    std::vector<CssSimpleSelector> parts;  // left-to-right
    std::vector<char> combinators;         // relation parts[i] -> parts[i + 1]
    int id_count = 0, class_count = 0, tag_count = 0;
    bool valid = false;
    // 'b'/'a' when the selector targets the last part's ::before/::after
    // pseudo-element (the pseudo itself is stripped from that part's pseudos,
    // so element matching works normally). The cascade routes such a rule's
    // `content` into ComputedStyle::content_before/content_after instead of
    // applying its declarations to the element; querySelector never matches
    // pseudo-element selectors, same as a real browser.
    char pseudo_element = 0;
};

struct CssRule {
    std::string selector;
    std::unordered_map<std::string, std::string> decls;
    size_t source_order = 0;
    // Selector parsed ONCE when the rule is collected. The cascade used to
    // re-run ParseSelector per (node, rule) pair -- ~2000 nodes x ~150 rules
    // = 300k string parses per ComputeStyles, the whole reason a class
    // toggle (opening the nav drawer) took visible fractions of a second.
    ParsedSelector parsed;
};

bool IsCssIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

ParsedSelector ParseSelector(const std::string &raw) {
    ParsedSelector out;
    size_t i = 0;
    char pending = 0;
    auto spaces = [&]() { bool any = false; while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) { any = true; ++i; } return any; };
    spaces();
    while (i < raw.size()) {
        CssSimpleSelector simple;
        bool has_piece = false;
        if (raw[i] == '*') { ++i; has_piece = true; }
        else if (IsCssIdentChar(raw[i])) {
            size_t start = i; while (i < raw.size() && IsCssIdentChar(raw[i])) ++i;
            simple.tag = raw.substr(start, i - start); out.tag_count++; has_piece = true;
        }
        while (i < raw.size()) {
            if (raw[i] == '#') {
                size_t start = ++i; while (i < raw.size() && IsCssIdentChar(raw[i])) ++i;
                if (start == i) return out;
                simple.id = raw.substr(start, i - start); out.id_count++; has_piece = true;
            } else if (raw[i] == '.') {
                size_t start = ++i; while (i < raw.size() && IsCssIdentChar(raw[i])) ++i;
                if (start == i) return out;
                simple.classes.push_back(raw.substr(start, i - start)); out.class_count++; has_piece = true;
            } else if (raw[i] == '[') {
                size_t close = raw.find(']', i + 1); if (close == std::string::npos) return out;
                std::string body = raw.substr(i + 1, close - i - 1);
                size_t begin = body.find_first_not_of(" \t"), end = body.find_last_not_of(" \t");
                if (begin == std::string::npos) return out;
                body = body.substr(begin, end - begin + 1);
                CssSimpleSelector::Attribute attr;
                // The full attribute-operator set: ~= (word), ^= (prefix),
                // $= (suffix), *= (substring), |= (exact or "value-" prefix),
                // plain = (exact). `a[href^="#"]` is how pages find their own
                // in-page anchor links (the scroll-spy rail idiom).
                size_t op = std::string::npos;
                for (const char *two : {"~=", "^=", "$=", "*=", "|="}) {
                    op = body.find(two);
                    if (op != std::string::npos) { attr.op = two[0]; break; }
                }
                if (op == std::string::npos) { op = body.find('='); if (op != std::string::npos) attr.op = '='; }
                attr.name = ToLower(body.substr(0, op == std::string::npos ? body.size() : op));
                if (op != std::string::npos) {
                    size_t value_at = op + (attr.op == '=' ? 1 : 2);
                    attr.value = body.substr(value_at);
                    if (attr.value.size() >= 2 && (attr.value.front() == '\'' || attr.value.front() == '"') && attr.value.back() == attr.value.front()) attr.value = attr.value.substr(1, attr.value.size() - 2);
                }
                if (attr.name.empty()) return out;
                simple.attributes.push_back(std::move(attr)); out.class_count++; has_piece = true; i = close + 1;
            } else if (raw[i] == ':') {
                ++i;
                if (i < raw.size() && raw[i] == ':') ++i;  // ::before / ::after pseudo-element syntax
                size_t name_start = i; while (i < raw.size() && IsCssIdentChar(raw[i])) ++i;
                if (name_start == i) return out;
                std::string name = raw.substr(name_start, i - name_start);
                std::string argument;
                if (i < raw.size() && raw[i] == '(') {
                    size_t close = raw.find(')', i + 1); if (close == std::string::npos) return out;
                    argument = raw.substr(i + 1, close - i - 1); i = close + 1;
                }
                simple.pseudos.push_back({std::move(name), std::move(argument)});
                out.class_count++; has_piece = true;
            } else break;
        }
        if (!has_piece) return out;
        if (!out.parts.empty()) out.combinators.push_back(pending ? pending : ' ');
        out.parts.push_back(std::move(simple)); pending = 0;
        bool had_space = spaces();
        if (i >= raw.size()) break;
        if (raw[i] == '>' || raw[i] == '+' || raw[i] == '~') { pending = raw[i++]; spaces(); }
        else if (had_space) pending = ' ';
        else return out;
    }
    // A trailing (::)before/after on the last compound targets a
    // pseudo-element: record it on the selector and strip it from the part so
    // element matching (MatchesSimple) sees only real pseudo-classes.
    if (!out.parts.empty()) {
        auto &pseudos = out.parts.back().pseudos;
        for (size_t p = 0; p < pseudos.size();) {
            if (pseudos[p].first == "before" || pseudos[p].first == "after") {
                out.pseudo_element = pseudos[p].first[0] == 'b' ? 'b' : 'a';
                pseudos.erase(pseudos.begin() + static_cast<std::ptrdiff_t>(p));
            } else {
                ++p;
            }
        }
    }
    out.valid = !out.parts.empty();
    return out;
}

bool MatchesSelectorAt(const DomNode *node, const ParsedSelector &selector, int part, const DomNode *scope_root = nullptr);

// :focus-within -- `node` or anything inside it has focus.
bool HasFocusWithin(const DomNode *node) {
    if (node->interaction_focus) return true;
    for (const auto &child : node->children) if (HasFocusWithin(child.get())) return true;
    return false;
}

bool IsLastElementChild(const DomNode *node) {
    if (!node->parent) return true;
    for (auto it = node->parent->children.rbegin(); it != node->parent->children.rend(); ++it)
        if ((*it)->type == DomNodeType::Element) return it->get() == node;
    return true;
}

bool HasClass(const DomNode *node, const std::string &want) {
    // Plain scan, no istringstream: this is the hottest call in the whole
    // cascade (every class simple-selector for every node goes through it),
    // and a stream's constructor alone costs more than the entire match.
    const std::string &cls = node->Class();
    if (cls.empty() || want.empty()) return false;
    size_t i = 0;
    while (i < cls.size()) {
        while (i < cls.size() && std::isspace(static_cast<unsigned char>(cls[i]))) ++i;
        size_t start = i;
        while (i < cls.size() && !std::isspace(static_cast<unsigned char>(cls[i]))) ++i;
        if (i - start == want.size() && cls.compare(start, want.size(), want) == 0) return true;
    }
    return false;
}

int ElementIndex(const DomNode *node, bool same_type) {
    if (!node || !node->parent) return 0;
    int index = 0;
    for (const auto &child : node->parent->children) {
        if (child->type != DomNodeType::Element || (same_type && child->tag != node->tag)) continue;
        ++index;
        if (child.get() == node) return index;
    }
    return 0;
}

bool NthMatches(const std::string &raw, int index) {
    std::string v; for (char c : raw) if (!std::isspace(static_cast<unsigned char>(c))) v += c;
    if (v == "odd") return index % 2 == 1;
    if (v == "even") return index % 2 == 0;
    size_t n = v.find('n');
    if (n == std::string::npos) return index == std::atoi(v.c_str());
    int a = n == 0 ? 1 : (v.substr(0, n) == "-" ? -1 : std::atoi(v.substr(0, n).c_str()));
    int b = n + 1 == v.size() ? 0 : std::atoi(v.substr(n + 1).c_str());
    return a != 0 && (index - b) * a >= 0 && (index - b) % a == 0;
}

// `scope_root` is the element a `:scope`-relative query started from (the
// receiver of element.querySelector[All]); `:scope` matches only it. Null in
// the CSS-cascade context, where `:scope` degrades to `:root` (the <html>
// element) as the spec says for a scope-less match.
bool MatchesSimple(const DomNode *node, const CssSimpleSelector &simple, const DomNode *scope_root = nullptr) {
    if (!node || node->type != DomNodeType::Element) return false;
    if (!simple.tag.empty() && node->tag != simple.tag) return false;
    if (!simple.id.empty() && node->Id() != simple.id) return false;
    for (const std::string &klass : simple.classes) if (!HasClass(node, klass)) return false;
    for (const auto &attr : simple.attributes) {
        auto it = node->attrs.find(attr.name); if (it == node->attrs.end()) return false;
        const std::string &have = it->second;
        const std::string &want = attr.value;
        if (attr.op == '=' && have != want) return false;
        if (attr.op == '^' && (want.empty() || have.rfind(want, 0) != 0)) return false;
        if (attr.op == '$' && (want.empty() || have.size() < want.size() ||
                               have.compare(have.size() - want.size(), want.size(), want) != 0))
            return false;
        if (attr.op == '*' && (want.empty() || have.find(want) == std::string::npos)) return false;
        if (attr.op == '|' && have != want && have.rfind(want + "-", 0) != 0) return false;
        if (attr.op == '~') {
            std::istringstream words(have); std::string word; bool found = false;
            while (words >> word) if (word == want) { found = true; break; }
            if (!found) return false;
        }
    }
    for (const auto &[name, argument] : simple.pseudos) {
        if (name == "first-child" && ElementIndex(node, false) != 1) return false;
        if (name == "last-child") {
            int index = ElementIndex(node, false), count = 0;
            if (node->parent) for (const auto &child : node->parent->children) if (child->type == DomNodeType::Element) ++count;
            if (index != count) return false;
        }
        if (name == "nth-child" && !NthMatches(argument, ElementIndex(node, false))) return false;
        if (name == "nth-of-type" && !NthMatches(argument, ElementIndex(node, true))) return false;
        else if (name == "hover") { if (!node->interaction_hover) return false; }
        else if (name == "focus" || name == "focus-visible") { if (!node->interaction_focus) return false; }
        else if (name == "active") { if (!node->interaction_active) return false; }
        else if (name == "focus-within") { if (!HasFocusWithin(node)) return false; }
        else if (name == "disabled" || name == "enabled") {
            const bool control = node->tag == "button" || node->tag == "input" || node->tag == "select" || node->tag == "textarea";
            const bool disabled = control && node->attrs.count("disabled");
            if (!control || disabled != (name == "disabled")) return false;
        }
        else if (name == "checked") { if (!node->form_checked) return false; }
        else if (name == "root") { if (node->tag != "html") return false; }
        else if (name == "only-child") { if (ElementIndex(node, false) != 1 || !IsLastElementChild(node)) return false; }
        else if (name == "first-of-type") { if (ElementIndex(node, true) != 1) return false; }
        else if (name == "empty") { if (!node->children.empty()) return false; }
        else if (name == "link" || name == "any-link") { if (node->tag != "a" || !node->attrs.count("href")) return false; }
        else if (name == "scope") { if (scope_root ? (node != scope_root) : (node->tag != "html")) return false; }
        else if (name == "not") {
            const ParsedSelector inner = ParseSelector(argument);
            if (!inner.valid || MatchesSelectorAt(node, inner, static_cast<int>(inner.parts.size()) - 1, scope_root)) return false;
        }
        // One this does not know (:visited, :last-of-type, ...) matches
        // nothing, rather than everything: `button:disabled` must not
        // style every button.
        else if (name != "first-child" && name != "last-child" && name != "nth-child" && name != "nth-of-type") return false;
    }
    return true;
}

bool MatchesSelectorAt(const DomNode *node, const ParsedSelector &selector, int part,
                       const DomNode *scope_root) {
    if (!MatchesSimple(node, selector.parts[static_cast<size_t>(part)], scope_root)) return false;
    if (part == 0) return true;
    char combinator = selector.combinators[static_cast<size_t>(part - 1)];
    if (combinator == '>') return MatchesSelectorAt(node->parent, selector, part - 1, scope_root);
    if (combinator == ' ') {
        for (const DomNode *ancestor = node->parent; ancestor; ancestor = ancestor->parent)
            if (MatchesSelectorAt(ancestor, selector, part - 1, scope_root)) return true;
        return false;
    }
    if (!node->parent) return false;
    const auto &siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i) if (siblings[i].get() == node) {
        if (combinator == '+') {
            while (i > 0) { --i; if (siblings[i]->type == DomNodeType::Element) return MatchesSelectorAt(siblings[i].get(), selector, part - 1, scope_root); }
            return false;
        }
        while (i > 0) { --i; if (siblings[i]->type == DomNodeType::Element && MatchesSelectorAt(siblings[i].get(), selector, part - 1, scope_root)) return true; }
        return false;
    }
    return false;
}

// `scope_root` fixes what `:scope` matches for the whole walk (the element the
// query was rooted at); the recursion's own `node` is just the current cursor.
void CollectSelectorMatches(const DomNode *scope_root, DomNode *node, const ParsedSelector &selector,
                            std::vector<DomNode *> &out) {
    if (!node) return;
    if (node->type == DomNodeType::Element &&
        MatchesSelectorAt(node, selector, static_cast<int>(selector.parts.size()) - 1, scope_root))
        out.push_back(node);
    for (auto &child : node->children) CollectSelectorMatches(scope_root, child.get(), selector, out);
}

/**
 * @brief Parses a semicolon-separated "prop: value" declaration block into a property-name-to-value map, lowercasing and trimming both sides.
 * @param body Declaration block text (the contents between a CSS rule's braces, or an inline style="" attribute value).
 * @return Map of lowercased property names to lowercased, trimmed values.
 */
// Removes CSS `/* ... */` comments. CSS has no line comments, and none of
// the stylesheets this renderer sees embed "/*" inside a string/url(), so a
// plain scan is enough. Load-bearing: a comment anywhere in a <style> block
// (author stylesheets are full of them) otherwise gets glued into the next
// selector's text -- breaking that rule and, worse, throwing off the
// brace-matching for everything after it.
std::string StripCssComments(const std::string &css) {
    std::string out;
    out.reserve(css.size());
    for (size_t i = 0; i < css.size();) {
        if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            size_t end = css.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> ParseDeclarations(const std::string &raw_body) {
    const std::string body = StripCssComments(raw_body);
    std::unordered_map<std::string, std::string> out;
    size_t i = 0, n = body.size();
    while (i < n) {
        size_t colon = body.find(':', i);
        if (colon == std::string::npos) break;
        size_t semi = body.find(';', colon);
        std::string prop = body.substr(i, colon - i);
        std::string val = body.substr(colon + 1, (semi == std::string::npos ? n : semi) - colon - 1);
        /**
         * @brief Trims leading and trailing whitespace (spaces, tabs, CR, LF) from a string.
         * @param s String to trim.
         * @return The trimmed string, or an empty string if `s` is all whitespace.
         */
        auto trim = [](const std::string &s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        prop = ToLower(trim(prop));
        val = ToLower(trim(val));
        if (!prop.empty()) out[prop] = val;
        i = (semi == std::string::npos) ? n : semi + 1;
    }
    return out;
}

// #rgb / #rrggbb / a small named-color table -- CSS's full color grammar
// (rgb()/hsl()/alpha channels/currentColor/...) is out of scope; anything
// this doesn't recognize is silently ignored (the property just isn't
// set), same tolerance as an unmatched entity in DecodeEntities above.
/**
 * @brief Parses a CSS color value (#rgb, #rrggbb, or a small named-color table) into RGB components.
 * @param raw Raw CSS color value text.
 * @param r Set to the parsed red component on success.
 * @param g Set to the parsed green component on success.
 * @param b Set to the parsed blue component on success.
 * @return True if `raw` was recognized and `r`/`g`/`b` were set; false otherwise (left untouched).
 */
bool ParseColor(const std::string &raw, unsigned char *r, unsigned char *g, unsigned char *b,
                unsigned char *a = nullptr) {
    std::string v = raw;
    if (a) *a = 255;
    // rgb(r, g, b) / rgba(r, g, b, alpha) -- the alpha channel matters for
    // shadows and dimming backdrops (`--shadow: rgba(0,0,0,.5)`); callers
    // that pass no `a` slot just get the opaque components.
    if (v.rfind("rgb", 0) == 0) {
        size_t open = v.find('('), close = v.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) return false;
        std::string body = v.substr(open + 1, close - open - 1);
        for (char &c : body) if (c == ',' || c == '/') c = ' ';
        std::istringstream parts(body);
        double rr = 0, gg = 0, bb = 0, aa = 1.0;
        if (!(parts >> rr >> gg >> bb)) return false;
        parts >> aa;  // optional; stays 1.0 when absent
        auto clamp255 = [](double x) {
            return static_cast<unsigned char>(std::clamp(x, 0.0, 255.0));
        };
        *r = clamp255(rr);
        *g = clamp255(gg);
        *b = clamp255(bb);
        if (a) *a = static_cast<unsigned char>(std::clamp(aa, 0.0, 1.0) * 255.0 + 0.5);
        return true;
    }
    if (!v.empty() && v[0] == '#') {
        v = v.substr(1);
        /**
         * @brief Converts a single hex digit character to its numeric value.
         * @param c Hex digit character ('0'-'9' or 'a'-'f').
         * @return The digit's value (0-15), or -1 if `c` isn't a recognized hex digit.
         */
        auto hex1 = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        if (v.size() == 3) {
            int rr = hex1(v[0]), gg = hex1(v[1]), bb = hex1(v[2]);
            if (rr < 0 || gg < 0 || bb < 0) return false;
            *r = static_cast<unsigned char>(rr * 17);
            *g = static_cast<unsigned char>(gg * 17);
            *b = static_cast<unsigned char>(bb * 17);
            return true;
        }
        if (v.size() == 6) {
            int rr = hex1(v[0]) * 16 + hex1(v[1]);
            int gg = hex1(v[2]) * 16 + hex1(v[3]);
            int bb = hex1(v[4]) * 16 + hex1(v[5]);
            if (rr < 0 || gg < 0 || bb < 0) return false;
            *r = static_cast<unsigned char>(rr);
            *g = static_cast<unsigned char>(gg);
            *b = static_cast<unsigned char>(bb);
            return true;
        }
        return false;
    }
    static const std::unordered_map<std::string, unsigned int> kNamed = {
        {"black", 0x000000},   {"white", 0xffffff}, {"red", 0xdc3232},    {"green", 0x2e9e4a},
        {"blue", 0x3c78dc},    {"yellow", 0xd7c832}, {"orange", 0xe08a2d}, {"purple", 0x9b5bc8},
        {"gray", 0x888888},    {"grey", 0x888888},   {"pink", 0xe085a8},   {"brown", 0x8a5a3c},
        {"cyan", 0x40c8c8},    {"magenta", 0xc850c8}, {"navy", 0x2a3f8f},  {"teal", 0x2f8f8f},
        {"maroon", 0x8f2a3f},  {"olive", 0x8f8f2a},   {"silver", 0xc0c0c0}, {"lime", 0x60d060},
    };
    auto it = kNamed.find(v);
    if (it == kNamed.end()) return false;
    *r = static_cast<unsigned char>((it->second >> 16) & 0xff);
    *g = static_cast<unsigned char>((it->second >> 8) & 0xff);
    *b = static_cast<unsigned char>(it->second & 0xff);
    return true;
}

// Parses one border side's compound value ("1px solid #d0d7de", "none",
// "border:none", any token order) into `edge`. CSS lets width/style/color
// appear in any order and any subset be omitted; since this renderer only
// ever draws a border as a solid line, the style keyword itself (solid/
// dashed/double/...) is recognized just well enough to detect "none"/
// "hidden" (no border at all) and otherwise ignored. Width defaults to 1px
// if the value has a color/style but no explicit width (mirrors CSS's own
// "medium" default closely enough for this renderer's purposes). Leaves
// `edge` untouched (still absent) if nothing recognizable was found.
/**
 * @brief Parses one border side's compound value (width/style/color in any order/subset, or "none"/"hidden") into a BorderEdge; every border still draws as a solid line regardless of the style keyword.
 * @param val Compound border value text (e.g. "1px solid #d0d7de").
 * @param edge Border edge updated in place; left untouched if nothing recognizable was found.
 */
void ParseBorderEdge(const std::string &val, ComputedStyle::BorderEdge &edge) {
    std::istringstream iss(val);
    std::string tok;
    bool any = false;
    edge.width_px = 1.0f;
    while (iss >> tok) {
        if (tok == "none" || tok == "hidden") {
            edge.present = false;
            return;
        }
        char *end = nullptr;
        double num = std::strtod(tok.c_str(), &end);
        if (end != tok.c_str()) {
            edge.width_px = static_cast<float>(num);
            any = true;
            continue;
        }
        unsigned char r, g, b;
        if (ParseColor(tok, &r, &g, &b)) {
            edge.r = r;
            edge.g = g;
            edge.b = b;
            any = true;
            continue;
        }
        // Otherwise a style keyword (solid/dashed/double/groove/...) --
        // recognized as "this token belongs to a border value" but not
        // otherwise distinguished, see this function's own header comment.
        any = true;
    }
    if (any) edge.present = true;
}

// `allow_negative` is for the properties CSS itself allows below zero
// (transform translations; box lengths like width/padding stay invalid when
// negative, matching the spec, so a page's `-1px` typo can't corrupt layout).
bool ParseCssLength(const std::string &raw, CssLength &out, bool allow_auto = false, bool allow_negative = false) {
    std::string v = raw;
    size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    v = v.substr(a, b - a + 1);
    if (allow_auto && v == "auto") {
        out = CssLength{};
        out.set = true;
        out.auto_value = true;
        return true;
    }
    // calc() with only px/rem terms constant-folds to a px length right here:
    // in this engine 1rem is always 16 CSS px (layout scales the whole px
    // space by the real root font later, ResolveCssLength in main.cpp), so
    // `calc(220px - 2.3rem)` is 183.2px at parse time. Terms in other units
    // (%/em/vw) would need layout context, so such a calc stays unparsed.
    if (v.rfind("calc(", 0) == 0 && v.back() == ')') {
        std::string body = v.substr(5, v.size() - 6);
        float total_px = 0.0f;
        float sign = 1.0f;
        size_t i = 0;
        bool ok = !body.empty();
        while (ok && i < body.size()) {
            while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
            size_t start = i;
            while (i < body.size() && !std::isspace(static_cast<unsigned char>(body[i]))) ++i;
            std::string token = body.substr(start, i - start);
            if (token.empty()) break;
            if (token == "+") { continue; }
            if (token == "-") { sign = -sign; continue; }
            CssLength term;
            if (!ParseCssLength(token, term, false, true) ||
                (term.unit != CssLength::Unit::Px && term.unit != CssLength::Unit::Rem)) {
                ok = false;
                break;
            }
            total_px += sign * (term.unit == CssLength::Unit::Rem ? term.value * 16.0f : term.value);
            sign = 1.0f;
        }
        if (!ok || (total_px < 0.0f && !allow_negative)) return false;
        out.set = true;
        out.auto_value = false;
        out.value = total_px;
        out.unit = CssLength::Unit::Px;
        return true;
    }
    char *end = nullptr;
    double value = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || (value < 0.0 && !allow_negative)) return false;
    std::string suffix = end;
    CssLength::Unit unit = CssLength::Unit::Px;
    if (suffix.empty() || suffix == "px") unit = CssLength::Unit::Px;
    else if (suffix == "%") unit = CssLength::Unit::Percent;
    else if (suffix == "em") unit = CssLength::Unit::Em;
    else if (suffix == "rem") unit = CssLength::Unit::Rem;
    else if (suffix == "vw") unit = CssLength::Unit::Vw;
    else if (suffix == "vh") unit = CssLength::Unit::Vh;
    else return false;
    out.set = true;
    out.auto_value = false;
    out.value = static_cast<float>(value);
    out.unit = unit;
    return true;
}

void ParseCssEdges(const std::string &raw, CssEdges &edges, bool allow_auto) {
    std::istringstream stream(raw);
    std::vector<CssLength> values;
    std::string token;
    while (stream >> token && values.size() < 4) {
        CssLength value;
        if (!ParseCssLength(token, value, allow_auto)) return;
        values.push_back(value);
    }
    if (values.empty()) return;
    edges.top = values[0];
    edges.right = values.size() > 1 ? values[1] : values[0];
    edges.bottom = values.size() > 2 ? values[2] : values[0];
    edges.left = values.size() > 3 ? values[3] : edges.right;
}

/**
 * @brief Applies a parsed CSS declaration map to a ComputedStyle, handling color, background, border, font-weight/style, text-decoration, display, font-size, max-width, and horizontal-auto-margin properties.
 * @param s Style updated in place; only properties present in `decls` (and recognized) are overridden.
 * @param decls Property-name-to-value map, as produced by ParseDeclarations.
 */
void ApplyDeclarations(ComputedStyle &s, const std::unordered_map<std::string, std::string> &decls) {
    unsigned char r, g, b;
    if (auto it = decls.find("color"); it != decls.end() && ParseColor(it->second, &r, &g, &b)) {
        s.has_color = true;
        s.color_r = r;
        s.color_g = g;
        s.color_b = b;
    }
    for (const char *key : {"background-color", "background"}) {
        unsigned char bg_alpha = 255;
        if (auto it = decls.find(key); it != decls.end() && ParseColor(it->second, &r, &g, &b, &bg_alpha)) {
            s.has_bg = true;
            s.bg_r = r;
            s.bg_g = g;
            s.bg_b = b;
            s.bg_a = bg_alpha;
        }
    }
    if (auto it = decls.find("opacity"); it != decls.end()) {
        char *end = nullptr;
        double v = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str()) s.opacity = static_cast<float>(std::clamp(v, 0.0, 1.0));
    }
    if (auto it = decls.find("transition"); it != decls.end()) {
        // Only `transform <duration>` is modelled (the drawer slide). The
        // duration is the first "<number>s"/"<number>ms" token after the
        // word "transform" in that comma-separated entry.
        size_t at = it->second.find("transform");
        if (at != std::string::npos) {
            const std::string tail = it->second.substr(at);
            size_t i = 0;
            while (i < tail.size()) {
                if (std::isdigit(static_cast<unsigned char>(tail[i])) || tail[i] == '.') {
                    char *end = nullptr;
                    double v = std::strtod(tail.c_str() + i, &end);
                    std::string suffix(end);
                    if (suffix.rfind("ms", 0) == 0) { s.transition_transform_s = static_cast<float>(v / 1000.0); break; }
                    if (!suffix.empty() && suffix[0] == 's') { s.transition_transform_s = static_cast<float>(v); break; }
                    i = static_cast<size_t>(end - tail.c_str());
                } else if (tail[i] == ',') {
                    break;  // next transition entry -- transform had no duration
                } else {
                    ++i;
                }
            }
        }
    }
    if (auto it = decls.find("border"); it != decls.end()) {
        ParseBorderEdge(it->second, s.border_top);
        s.border_right = s.border_top;
        s.border_bottom = s.border_top;
        s.border_left = s.border_top;
    }
    if (auto it = decls.find("border-top"); it != decls.end()) ParseBorderEdge(it->second, s.border_top);
    if (auto it = decls.find("border-right"); it != decls.end()) ParseBorderEdge(it->second, s.border_right);
    if (auto it = decls.find("border-bottom"); it != decls.end()) ParseBorderEdge(it->second, s.border_bottom);
    if (auto it = decls.find("border-left"); it != decls.end()) ParseBorderEdge(it->second, s.border_left);
    if (auto it = decls.find("border-width"); it != decls.end()) {
        CssEdges widths;
        ParseCssEdges(it->second, widths, false);
        const CssLength *v[4] = {&widths.top, &widths.right, &widths.bottom, &widths.left};
        ComputedStyle::BorderEdge *e[4] = {&s.border_top, &s.border_right, &s.border_bottom, &s.border_left};
        for (int i = 0; i < 4; ++i) if (v[i]->set && v[i]->unit == CssLength::Unit::Px) { e[i]->present = true; e[i]->width_px = v[i]->value; }
    }
    if (auto it = decls.find("border-color"); it != decls.end()) {
        std::istringstream stream(it->second);
        struct Rgb { unsigned char r, g, b; };
        std::vector<Rgb> colors;
        std::string token;
        while (stream >> token && colors.size() < 4) {
            unsigned char cr, cg, cb;
            if (!ParseColor(token, &cr, &cg, &cb)) { colors.clear(); break; }
            colors.push_back(Rgb{cr, cg, cb});
        }
        if (!colors.empty()) {
            ComputedStyle::BorderEdge *e[4] = {&s.border_top, &s.border_right, &s.border_bottom, &s.border_left};
            for (int i = 0; i < 4; ++i) {
                size_t index = 0;
                if (i == 1 || i == 3) index = std::min<size_t>(1, colors.size() - 1);
                else if (i == 2) index = std::min<size_t>(2, colors.size() - 1);
                if (i == 3 && colors.size() == 4) index = 3;
                const Rgb c = colors[index];
                e[i]->present = true; e[i]->r = c.r; e[i]->g = c.g; e[i]->b = c.b;
            }
        }
    }
    for (const auto &[name, edge] : std::initializer_list<std::pair<const char *, ComputedStyle::BorderEdge *>>{
             {"border-top", &s.border_top}, {"border-right", &s.border_right},
             {"border-bottom", &s.border_bottom}, {"border-left", &s.border_left}}) {
        if (auto it = decls.find(std::string(name) + "-width"); it != decls.end()) {
            CssLength width;
            if (ParseCssLength(it->second, width) && width.unit == CssLength::Unit::Px) { edge->present = true; edge->width_px = width.value; }
        }
        if (auto it = decls.find(std::string(name) + "-color"); it != decls.end() && ParseColor(it->second, &r, &g, &b)) {
            edge->present = true; edge->r = r; edge->g = g; edge->b = b;
        }
    }
    if (auto it = decls.find("margin"); it != decls.end()) ParseCssEdges(it->second, s.margin, true);
    if (auto it = decls.find("padding"); it != decls.end()) ParseCssEdges(it->second, s.padding, false);
    for (const auto &[name, target] : std::initializer_list<std::pair<const char *, CssLength *>>{
             {"margin-top", &s.margin.top}, {"margin-right", &s.margin.right}, {"margin-bottom", &s.margin.bottom}, {"margin-left", &s.margin.left},
             {"padding-top", &s.padding.top}, {"padding-right", &s.padding.right}, {"padding-bottom", &s.padding.bottom}, {"padding-left", &s.padding.left},
             {"width", &s.width}, {"height", &s.height}, {"min-width", &s.min_width}, {"min-height", &s.min_height},
             {"max-width", &s.max_width}, {"max-height", &s.max_height}}) {
        if (auto it = decls.find(name); it != decls.end()) ParseCssLength(it->second, *target, std::string(name).find("margin") == 0);
    }
    if (auto it = decls.find("box-sizing"); it != decls.end()) s.border_box = it->second == "border-box";
    if (auto it = decls.find("text-align"); it != decls.end()) {
        if (it->second == "center") s.text_align = HtmlTextAlign::Center;
        else if (it->second == "right" || it->second == "end") s.text_align = HtmlTextAlign::Right;
        else if (it->second == "justify") s.text_align = HtmlTextAlign::Justify;
        else s.text_align = HtmlTextAlign::Left;
    }
    if (auto it = decls.find("white-space"); it != decls.end()) {
        if (it->second == "pre" || it->second == "pre-wrap") s.white_space = HtmlWhiteSpace::Pre;
        else if (it->second == "nowrap") s.white_space = HtmlWhiteSpace::NoWrap;
        else s.white_space = HtmlWhiteSpace::Normal;
    }
    // `list-style` (shorthand) / `list-style-type`: only the none-vs-marker
    // distinction matters here (the exact bullet glyph is always drawn as
    // "* " -- see main.cpp), so any value naming "none" suppresses the
    // marker and any other explicit type restores it. The shorthand can
    // also carry position/image tokens; a bare "none" in it means the type.
    for (const char *key : {"list-style-type", "list-style"}) {
        if (auto it = decls.find(key); it != decls.end()) {
            const std::string v = ToLower(it->second);
            if (v.find("none") != std::string::npos) s.list_marker_none = true;
            else s.list_marker_none = false;
        }
    }
    if (auto it = decls.find("letter-spacing"); it != decls.end()) ParseCssLength(it->second, s.letter_spacing);
    if (auto it = decls.find("line-height"); it != decls.end()) {
        char *end = nullptr;
        const double value = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str() && *end == '\0' && value > 0.0) {
            s.line_height_multiplier = static_cast<float>(value);
            s.line_height_length = CssLength{};
        } else if (ParseCssLength(it->second, s.line_height_length)) {
            s.line_height_multiplier = 0.0f;
        }
    }
    if (auto it = decls.find("font-family"); it != decls.end()) {
        std::string family = ToLower(it->second);
        // CSS font-family is an ordered fallback list.  We only ship the
        // three generic faces, so select the first generic family we know;
        // an otherwise unknown name lands on the standard sans fallback.
        if (family.find("monospace") != std::string::npos) s.font_family = HtmlFontFamily::Mono;
        else if (family.find("sans-serif") != std::string::npos || family.find("sans") != std::string::npos)
            s.font_family = HtmlFontFamily::Sans;
        else if (family.find("serif") != std::string::npos) s.font_family = HtmlFontFamily::Serif;
        else s.font_family = HtmlFontFamily::Sans;
    }
    if (auto it = decls.find("font-weight"); it != decls.end()) {
        const std::string &v = it->second;
        if (v == "bold" || v == "bolder" || (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0])) && v >= "600")) {
            s.bold = true;
        } else if (v == "normal") {
            s.bold = false;
        }
    }
    if (auto it = decls.find("font-style"); it != decls.end()) {
        s.italic = (it->second == "italic" || it->second == "oblique");
    }
    for (const char *key : {"text-decoration", "text-decoration-line"}) {
        if (auto it = decls.find(key); it != decls.end()) {
            if (it->second.find("underline") != std::string::npos) s.underline = true;
            if (it->second.find("line-through") != std::string::npos) s.strikethrough = true;
            if (it->second == "none") {
                s.underline = false;
                s.strikethrough = false;
            }
        }
    }
    if (auto it = decls.find("display"); it != decls.end()) {
        // A later `display` declaration REPLACES an earlier one entirely --
        // the ubiquitous "hidden by default, revealed by a media/state rule"
        // pattern is `display:none` in one rule and `display:flex` in a more
        // specific one, so any non-none value must clear display_none, not
        // leave it latched from the earlier layer.
        s.display_none = it->second == "none";
        s.flex_container = it->second == "flex" || it->second == "inline-flex";
        if (it->second == "block" || it->second == "list-item" || it->second == "flex" ||
            it->second == "grid" || it->second == "table")
            s.block = true;
        else if (it->second == "inline" || it->second == "inline-block" || it->second == "inline-flex" ||
                 it->second == "inline-grid")
            s.block = false;
    }
    if (auto it = decls.find("flex-direction"); it != decls.end())
        s.flex_column = it->second.find("column") != std::string::npos;
    if (auto it = decls.find("inset"); it != decls.end()) {
        // inset: <top> [<right> [<bottom> [<left>]]], the margin shorthand's order.
        std::istringstream parts(it->second);
        std::vector<std::string> values;
        for (std::string v; parts >> v;) values.push_back(v);
        if (!values.empty()) {
            const std::string &top = values[0];
            const std::string &right = values.size() >= 2 ? values[1] : top;
            const std::string &bottom = values.size() >= 3 ? values[2] : top;
            const std::string &left = values.size() >= 4 ? values[3] : right;
            ParseCssLength(top, s.pos_top); ParseCssLength(right, s.pos_right);
            ParseCssLength(bottom, s.pos_bottom); ParseCssLength(left, s.pos_left);
        }
    }
    if (auto it = decls.find("visibility"); it != decls.end()) {
        if (it->second == "hidden" || it->second == "collapse") s.visibility_hidden = true;
        else if (it->second == "visible") s.visibility_hidden = false;
    }
    if (auto it = decls.find("opacity"); it != decls.end()) {
        char *end = nullptr;
        const double value = std::strtod(it->second.c_str(), &end);
        if (end != it->second.c_str()) s.opacity_zero = (*end == '%' ? value / 100.0 : value) < 0.01;
    }
    if (auto it = decls.find("font-size"); it != decls.end()) {
        const std::string &v = it->second;
        char *end = nullptr;
        double num = std::strtod(v.c_str(), &end);
        if (end != v.c_str()) {
            if (v.find("em") != std::string::npos) s.font_scale = static_cast<float>(num);
            else if (v.find("px") != std::string::npos)
                s.font_scale = static_cast<float>(num / 16.0);  // 16px is the common CSS default root size
            else if (v == "larger")
                s.font_scale *= 1.2f;
            else if (v == "smaller")
                s.font_scale *= 0.85f;
        }
    }
    if (auto it = decls.find("max-width"); it != decls.end()) {
        const std::string &v = it->second;
        char *end = nullptr;
        double num = std::strtod(v.c_str(), &end);
        if (end != v.c_str()) {
            if (v.find("em") != std::string::npos) {
                s.has_max_width = true;
                s.max_width_em = static_cast<float>(num);
            } else if (v.find("px") != std::string::npos) {
                s.has_max_width = true;
                s.max_width_em = static_cast<float>(num / 16.0);  // 16px is the common CSS default root size
            }
            // % and other units aren't supported -- silently ignored, same
            // tolerance as an unrecognized font-size unit just above.
        }
    }
    // Only the common "margin: <v> auto" / "margin: <v> auto <v>" / explicit
    // margin-left:auto + margin-right:auto centering idiom is recognized --
    // detected as "does this declaration's value contain the auto token at
    // all", not a full 1-4-value margin shorthand parse (this renderer has
    // no other margin-left/right support to combine it with anyway). A
    // lone margin-left:auto with no matching margin-right is treated the
    // same as a real centering pair rather than a right-push, a deliberate
    // simplification -- see ComputedStyle::margin_h_auto's own comment.
    for (const char *key : {"margin", "margin-left", "margin-right"}) {
        if (auto it = decls.find(key); it != decls.end() && it->second.find("auto") != std::string::npos) {
            s.margin_h_auto = true;
        }
    }
    // Positioning. `position` and its offsets don't inherit, so a plain
    // assignment here (over the Static default WalkAndStyle seeded) is right.
    if (auto it = decls.find("position"); it != decls.end()) {
        const std::string &v = it->second;
        if (v.find("fixed") != std::string::npos) s.position = CssPosition::Fixed;
        else if (v.find("sticky") != std::string::npos) s.position = CssPosition::Sticky;
        else if (v.find("absolute") != std::string::npos) s.position = CssPosition::Absolute;
        else if (v.find("relative") != std::string::npos) s.position = CssPosition::Relative;
        else s.position = CssPosition::Static;
    }
    for (const auto &[name, target] : std::initializer_list<std::pair<const char *, CssLength *>>{
             {"top", &s.pos_top}, {"right", &s.pos_right}, {"bottom", &s.pos_bottom}, {"left", &s.pos_left}}) {
        if (auto it = decls.find(name); it != decls.end()) ParseCssLength(it->second, *target);
    }
    // Any non-visible overflow (hidden/auto/scroll, on either axis or the
    // shorthand) means "clip this box's content to its bounds" -- all this
    // renderer needs to keep a fixed sidebar's nowrap labels from spilling.
    for (const char *key : {"overflow", "overflow-x", "overflow-y"}) {
        if (auto it = decls.find(key); it != decls.end()) {
            const std::string &v = it->second;
            if (v.find("hidden") != std::string::npos || v.find("auto") != std::string::npos ||
                v.find("scroll") != std::string::npos)
                s.overflow_clip = true;
        }
    }
    if (auto it = decls.find("text-overflow"); it != decls.end() && it->second.find("ellipsis") != std::string::npos)
        s.text_overflow_ellipsis = true;
    if (auto it = decls.find("border-radius"); it != decls.end()) {
        // Only the shorthand's first value (uniform corners) is modelled.
        std::istringstream toks(it->second);
        std::string first;
        toks >> first;
        ParseCssLength(first, s.border_radius);
    }
    if (auto it = decls.find("box-shadow"); it != decls.end()) {
        if (it->second == "none") {
            s.has_shadow = false;
        } else {
            // Minimal `x y blur [spread] color` form: the 3rd length is the
            // blur, the color token is whatever ParseColor recognizes
            // (rgba(...) keeps its internal spaces -- tokenize paren-aware).
            std::vector<std::string> toks;
            std::string cur;
            int depth = 0;
            for (char c : it->second) {
                if (c == '(') depth++;
                if (c == ')') depth--;
                if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
                    if (!cur.empty()) toks.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) toks.push_back(std::move(cur));
            int length_index = 0;
            for (const std::string &tok : toks) {
                unsigned char sr = 0, sg = 0, sb = 0, sa = 255;
                CssLength len;
                if (ParseColor(tok, &sr, &sg, &sb, &sa)) {
                    s.shadow_r = sr; s.shadow_g = sg; s.shadow_b = sb; s.shadow_a = sa;
                    s.has_shadow = true;
                } else if (ParseCssLength(tok, len, false, /*allow_negative=*/true)) {
                    if (length_index == 2) { s.shadow_blur = len; s.has_shadow = true; }
                    ++length_index;
                }
            }
        }
    }
    if (auto it = decls.find("align-items"); it != decls.end())
        s.align_items_center = it->second.find("center") != std::string::npos;
    if (auto it = decls.find("justify-content"); it != decls.end())
        s.justify_content_center = it->second.find("center") != std::string::npos;
    if (auto it = decls.find("text-transform"); it != decls.end()) {
        if (it->second == "uppercase") s.text_transform = ComputedStyle::TextTransform::Upper;
        else if (it->second == "lowercase") s.text_transform = ComputedStyle::TextTransform::Lower;
        else if (it->second == "capitalize") s.text_transform = ComputedStyle::TextTransform::Capitalize;
        else s.text_transform = ComputedStyle::TextTransform::None;
    }
    for (const char *key : {"column-count", "columns"}) {
        if (auto it = decls.find(key); it != decls.end()) {
            char *end = nullptr;
            long n = std::strtol(it->second.c_str(), &end, 10);
            // `columns` can also carry a width ("columns: 12em 2") -- only a
            // leading integer count is taken; `auto`/width-only resets to 0.
            s.column_count = (end != it->second.c_str() && n >= 1 && n <= 12) ? static_cast<int>(n) : 0;
        }
    }
    if (auto it = decls.find("inset"); it != decls.end()) {
        // Shorthand for top/right/bottom/left, margin-style 1-4 value order.
        // `inset: 0` is the standard full-viewport overlay idiom.
        std::istringstream toks(it->second);
        std::vector<std::string> vals;
        std::string tok;
        while (toks >> tok && vals.size() < 4) vals.push_back(tok);
        if (!vals.empty()) {
            const std::string &top_v = vals[0];
            const std::string &right_v = vals.size() > 1 ? vals[1] : vals[0];
            const std::string &bottom_v = vals.size() > 2 ? vals[2] : vals[0];
            const std::string &left_v = vals.size() > 3 ? vals[3] : right_v;
            ParseCssLength(top_v, s.pos_top, true);
            ParseCssLength(right_v, s.pos_right, true);
            ParseCssLength(bottom_v, s.pos_bottom, true);
            ParseCssLength(left_v, s.pos_left, true);
        }
    }
    // `gap` doubles as the flex-row inter-item gap (main.cpp inserts a
    // spacer between inline-approximated flex items). Single-value gap sets
    // both axes; a two-value gap's SECOND value is the column (inline) gap.
    for (const char *key : {"gap", "column-gap"}) {
        if (auto it = decls.find(key); it != decls.end()) {
            std::istringstream toks(it->second);
            std::string tok, last;
            while (toks >> tok) last = tok;
            if (!last.empty()) ParseCssLength(last, s.column_gap);
        }
    }
    if (auto it = decls.find("border-collapse"); it != decls.end())
        s.border_collapse = it->second == "collapse";
    // transform: only translate* is modelled (see ComputedStyle). Values are
    // already lower-cased by ParseDeclarations. Percentages are kept unresolved
    // (they're relative to the element's own box, known only at layout).
    if (auto it = decls.find("transform"); it != decls.end() && it->second.find("none") == std::string::npos) {
        const std::string &v = it->second;
        auto fn_args = [&](const std::string &name) -> std::string {
            size_t p = v.find(name);
            if (p == std::string::npos) return {};
            size_t open = p + name.size() - 1;  // `name` includes the '('
            size_t close = v.find(')', open);
            return close == std::string::npos ? std::string{} : v.substr(open + 1, close - open - 1);
        };
        std::string tx = fn_args("translatex(");
        std::string ty = fn_args("translatey(");
        std::string tboth = fn_args("translate(");  // translate(x[, y])
        if (!tboth.empty()) {
            size_t comma = tboth.find(',');
            if (ParseCssLength(comma == std::string::npos ? tboth : tboth.substr(0, comma), s.transform_x,
                               /*allow_auto=*/false, /*allow_negative=*/true))
                s.has_transform = true;
            if (comma != std::string::npos && ParseCssLength(tboth.substr(comma + 1), s.transform_y,
                                                             /*allow_auto=*/false, /*allow_negative=*/true))
                s.has_transform = true;
        }
        if (!tx.empty() && ParseCssLength(tx, s.transform_x, /*allow_auto=*/false, /*allow_negative=*/true))
            s.has_transform = true;
        if (!ty.empty() && ParseCssLength(ty, s.transform_y, /*allow_auto=*/false, /*allow_negative=*/true))
            s.has_transform = true;
    }
}

// The viewport/features `@media` queries are evaluated against. Set by
// SetCssMediaContext before ComputeStyles runs (main.cpp re-styles a browser
// pane whenever its width or theme changes). Defaults to a desktop width so a
// page styled before any pane size is known doesn't briefly get its mobile
// layout. `viewport_h` is tracked too for `min-height`/`max-height`.
struct CssMediaContext {
    float viewport_w = 1280.0f;
    float viewport_h = 800.0f;
    bool dark = false;
};
CssMediaContext &MediaContext() {
    static CssMediaContext ctx;
    return ctx;
}
}  // namespace

// Bumped whenever the media context actually changes -- part of the
// rule-cache key, since @media gating decides WHICH rules get collected.
size_t g_css_media_version = 1;

void SetCssMediaContext(float viewport_w, float viewport_h, bool dark) {
    CssMediaContext &ctx = MediaContext();
    if (ctx.viewport_w != viewport_w || ctx.viewport_h != viewport_h || ctx.dark != dark) ++g_css_media_version;
    ctx.viewport_w = viewport_w;
    ctx.viewport_h = viewport_h;
    ctx.dark = dark;
}

namespace {

// Whether an @media prelude ("@media screen and (max-width: 600px)")
// holds for this renderer: a screen, the pane's viewport and colour scheme
// (MediaContext, set by SetCssMediaContext). A comma list holds when any
// part does; a feature this does not know makes its part fail, so a print
// block never leaks into the page.
bool MediaQueryApplies(const std::string &prelude) {
    const std::string q = ToLower(prelude.substr(prelude.find("media") + 5));
    const CssMediaContext &mc = MediaContext();
    auto part_applies = [&mc](std::string part) {
        bool negate = false;
        auto word_at_start = [&](const char *w) {
            const size_t a = part.find_first_not_of(" \t\r\n");
            const size_t n = std::strlen(w);
            if (a == std::string::npos || part.compare(a, n, w) != 0) return false;
            part.erase(0, a + n);
            return true;
        };
        if (word_at_start("not")) negate = true;
        else word_at_start("only");
        bool ok = true;
        const size_t a = part.find_first_not_of(" \t\r\n");
        if (a != std::string::npos && part[a] != '(') {
            size_t b = a;
            while (b < part.size() && std::isalpha(static_cast<unsigned char>(part[b]))) ++b;
            const std::string type = part.substr(a, b - a);
            ok = type == "screen" || type == "all";
            part.erase(0, b);
        }
        for (size_t open = part.find('('); ok && open != std::string::npos; open = part.find('(', open + 1)) {
            const size_t close = part.find(')', open);
            if (close == std::string::npos) { ok = false; break; }
            std::string feature = part.substr(open + 1, close - open - 1);
            feature.erase(std::remove_if(feature.begin(), feature.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), feature.end());
            const size_t colon = feature.find(':');
            const std::string name = feature.substr(0, colon), value = colon == std::string::npos ? "" : feature.substr(colon + 1);
            if (name == "prefers-color-scheme") ok = value == (mc.dark ? "dark" : "light");
            else if (name == "prefers-reduced-motion") ok = value == "no-preference";
            else if (name == "hover" || name == "any-hover") ok = value == "hover";
            else if (name == "pointer" || name == "any-pointer") ok = value == "fine";
            else if (name == "min-width" || name == "max-width" || name == "min-height" || name == "max-height") {
                char *end = nullptr;
                double px = std::strtod(value.c_str(), &end);
                if (end == value.c_str()) { ok = false; break; }
                if (std::strncmp(end, "em", 2) == 0 || std::strncmp(end, "rem", 3) == 0) px *= 16.0;
                const double have = static_cast<double>(name.find("width") != std::string::npos ? mc.viewport_w : mc.viewport_h);
                ok = name.compare(0, 3, "min") == 0 ? have >= px : have <= px;
            } else ok = false;
        }
        return negate ? !ok : ok;
    };
    size_t from = 0;
    while (true) {
        const size_t comma = q.find(',', from);
        if (part_applies(q.substr(from, comma == std::string::npos ? std::string::npos : comma - from))) return true;
        if (comma == std::string::npos) return false;
        from = comma + 1;
    }
}

/**
 * @brief Parses a (comment-stripped) CSS block into rules, appended to `rules`, splitting comma-separated selector lists into individual rules and recursing through nested at-rule blocks.
 * @param css Comment-stripped CSS source (a whole <style> body, or the inner body of an at-rule).
 * @param rules Output list appended with one CssRule per selector found.
 *
 * Brace-aware, unlike a flat find('{')/find('}') scan: it matches each
 * block's braces so a nested block (a `@media`/`@supports` wrapping other
 * rules, `@keyframes` with per-stop blocks) can't throw off the rules that
 * follow it. A `@media` body is included only when MediaQueryApplies says its
 * condition holds for the current viewport (SetCssMediaContext) -- so a page's
 * mobile `max-width` rules don't fire on a wide pane, nor its desktop
 * `min-width` rules on a narrow one. `@supports`/`@container`/`@layer` bodies
 * are still parsed as if top-level (their conditions are assumed to hold);
 * every other at-rule (`@keyframes`/`@font-face`/`@page`/`@import`/`@charset`)
 * is skipped whole (its declarations aren't ones this renderer applies, and
 * skipping the block keeps the brace matching aligned).
 */
void CollectCssRules(const std::string &css, std::vector<CssRule> &rules) {
    size_t i = 0, len = css.size();
    while (i < len) {
        while (i < len && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= len) break;
        size_t brace = css.find('{', i);
        if (brace == std::string::npos) break;
        // A statement at-rule (`@import ...;`, `@charset ...;`) ends at a
        // semicolon before any block -- consume it and move on.
        size_t semi = css.find(';', i);
        if (semi != std::string::npos && semi < brace) { i = semi + 1; continue; }
        std::string prelude = css.substr(i, brace - i);
        size_t pa = prelude.find_first_not_of(" \t\r\n");
        std::string head = (pa == std::string::npos) ? std::string() : prelude.substr(pa);
        // Match this block's braces to find its true end (nested blocks and all).
        size_t depth = 1, j = brace + 1;
        for (; j < len && depth > 0; ++j) {
            if (css[j] == '{') ++depth;
            else if (css[j] == '}') --depth;
        }
        const size_t body_len = (j > brace + 1) ? (j - 1 - (brace + 1)) : 0;  // exclude the closing '}'
        std::string body = css.substr(brace + 1, body_len);
        if (!head.empty() && head[0] == '@') {
            std::string keyword = ToLower(head.substr(1, head.find_first_of(" \t\r\n({", 1) - 1));
            if (keyword == "media") {
                if (MediaQueryApplies(head)) CollectCssRules(body, rules);
            } else if (keyword == "supports" || keyword == "container" || keyword == "layer" ||
                       keyword == "document" || keyword == "scope") {
                CollectCssRules(body, rules);
            }
            // else: keyframes/font-face/page/... -- skipped whole.
        } else {
            auto decls = ParseDeclarations(body);
            size_t s = 0;
            while (s < head.size()) {
                size_t comma = head.find(',', s);
                std::string one = head.substr(s, (comma == std::string::npos ? head.size() : comma) - s);
                size_t a = one.find_first_not_of(" \t\r\n");
                size_t b = one.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) {
                    CssRule rule{ToLower(one.substr(a, b - a + 1)), decls, rules.size(), {}};
                    rule.parsed = ParseSelector(rule.selector);
                    rules.push_back(std::move(rule));
                }
                if (comma == std::string::npos) break;
                s = comma + 1;
            }
        }
        i = j;
    }
}

/**
 * @brief Recursively collects CSS rules from every <style> element's text content in the subtree rooted at `n`.
 * @param n Root of the subtree to scan.
 * @param rules Output list appended with one CssRule per selector found.
 */
void CollectStyleRules(DomNode *n, std::vector<CssRule> &rules) {
    if (n->type == DomNodeType::Element && n->tag == "style") {
        std::string css;
        for (const auto &c : n->children) {
            if (c->type == DomNodeType::Text) css += c->text;
        }
        CollectCssRules(StripCssComments(css), rules);
    }
    for (auto &c : n->children) CollectStyleRules(c.get(), rules);
}

// Applies to `style` (the ComputedStyle WalkAndStyle is still building up
// for `n`), not `n->style` directly -- `n->style` still holds n's *old*
// style at this point (or a default-constructed one, for a fresh parse)
// and gets overwritten wholesale by WalkAndStyle's own `n->style = s;`
// right after both passes run, which would silently discard whatever this
// wrote there instead.
/**
 * @brief Applies every matching CSS rule in specificity/source order.
 */
// Substitutes CSS `var(--name[, fallback])` references in `value` using the
// custom properties in `vars`. Recurses (depth-limited, so a variable that
// refers to itself can't loop) since a variable's value can itself use
// var(). An unresolved reference with no fallback yields the empty string,
// which the property parsers then ignore -- the same "unsupported value is
// silently dropped" tolerance the rest of this file has.
std::string ResolveCssVars(const std::string &value, const std::unordered_map<std::string, std::string> &vars,
                            int depth = 0) {
    if (depth > 16 || value.find("var(") == std::string::npos) return value;
    std::string out;
    size_t i = 0;
    while (i < value.size()) {
        size_t at = value.find("var(", i);
        if (at == std::string::npos) { out += value.substr(i); break; }
        out += value.substr(i, at - i);
        // Find the matching close paren for this var( (values can nest parens).
        size_t j = at + 4, depth_p = 1;
        for (; j < value.size() && depth_p > 0; ++j) {
            if (value[j] == '(') ++depth_p;
            else if (value[j] == ')') --depth_p;
        }
        const size_t inner_end = (j > 0) ? j - 1 : value.size();  // index of the matching ')'
        std::string inner = value.substr(at + 4, inner_end - (at + 4));
        size_t comma = inner.find(',');
        std::string name = inner.substr(0, comma);
        std::string fallback = (comma == std::string::npos) ? std::string() : inner.substr(comma + 1);
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        name = trim(name);
        auto found = vars.find(name);
        if (found != vars.end()) out += ResolveCssVars(found->second, vars, depth + 1);
        else out += ResolveCssVars(trim(fallback), vars, depth + 1);
        i = j;
    }
    return out;
}

// Decodes a CSS `content` property value to the literal UTF-8 text it
// produces: strips the surrounding quotes and resolves backslash escapes
// (`\2039` hex-with-optional-trailing-space, `\"` literal). `none`/`normal`/
// unquoted values produce "" -- only plain string content is modelled
// (no counters/attr()/url()).
std::string DecodeCssContent(const std::string &raw) {
    size_t a = raw.find_first_not_of(" \t\r\n"), b = raw.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::string v = raw.substr(a, b - a + 1);
    if (v.size() < 2 || (v.front() != '"' && v.front() != '\'') || v.back() != v.front()) return "";
    v = v.substr(1, v.size() - 2);
    std::string out;
    for (size_t i = 0; i < v.size();) {
        if (v[i] != '\\') { out += v[i++]; continue; }
        ++i;
        if (i >= v.size()) break;
        if (std::isxdigit(static_cast<unsigned char>(v[i]))) {
            size_t start = i, len = 0;
            while (i < v.size() && len < 6 && std::isxdigit(static_cast<unsigned char>(v[i]))) { ++i; ++len; }
            long cp = std::strtol(v.substr(start, len).c_str(), nullptr, 16);
            if (i < v.size() && v[i] == ' ') ++i;  // an escape may be terminated by one space
            if (cp > 0 && cp < 0x110000 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
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
            }
        } else {
            out += v[i++];  // \" \' \\ and any other escaped literal
        }
    }
    return out;
}

// Cascade for one element: gather every matching rule (plus the element's
// inline style) in specificity/source order, then apply. Custom properties
// are handled in two passes so var() resolves against the element's *final*
// variable set: pass 1 folds every `--x` declaration into `vars` (later wins,
// seeded by the inherited `vars` the caller passed in), pass 2 applies the
// normal properties with their var() references substituted. `vars` is
// updated in place so the caller can pass it down to this element's children,
// which is how custom properties inherit.
// `vars` enters pointing at the INHERITED custom-property map and is only
// swapped to a private copy (built in `own_vars`) when this element actually
// declares a `--x` of its own -- most elements just share their ancestor's
// map, which is what makes the cascade affordable (the old copy-per-node was
// ~2000 map clones per ComputeStyles).
void ApplyMatchingRules(const DomNode *n, ComputedStyle &style, const std::vector<CssRule> &rules,
                         const std::unordered_map<std::string, std::string> &inline_decls,
                         const std::unordered_map<std::string, std::string> *&vars_ptr,
                         std::unordered_map<std::string, std::string> &own_vars) {
    struct Match { const CssRule *rule; const ParsedSelector *selector; };
    std::vector<Match> matches;
    for (const CssRule &r : rules) {
        // r.parsed was built once when the rule was collected -- matching is
        // pure reads from here on (the per-pair ParseSelector this replaces
        // dominated ComputeStyles' whole runtime).
        if (r.parsed.valid && MatchesSelectorAt(n, r.parsed, static_cast<int>(r.parsed.parts.size()) - 1))
            matches.push_back({&r, &r.parsed});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.selector->id_count != b.selector->id_count) return a.selector->id_count < b.selector->id_count;
        if (a.selector->class_count != b.selector->class_count) return a.selector->class_count < b.selector->class_count;
        if (a.selector->tag_count != b.selector->tag_count) return a.selector->tag_count < b.selector->tag_count;
        return a.rule->source_order < b.rule->source_order;
    });
    // Cascade layers, lowest priority first: matched rules (already sorted),
    // then the inline style="" (always wins over any rule). A ::before/
    // ::after rule is NOT a layer on the element itself -- only its `content`
    // is captured, into the pseudo slots (later matches win, same order as
    // the layers). `content: none`/"" clears an earlier match's value.
    std::vector<const std::unordered_map<std::string, std::string> *> layers;
    layers.reserve(matches.size() + 1);
    for (const Match &match : matches) {
        if (match.selector->pseudo_element) {
            if (auto content = match.rule->decls.find("content"); content != match.rule->decls.end()) {
                std::string &slot = match.selector->pseudo_element == 'b' ? style.content_before : style.content_after;
                slot = DecodeCssContent(content->second);
            }
            continue;
        }
        layers.push_back(&match.rule->decls);
    }
    if (!inline_decls.empty()) layers.push_back(&inline_decls);

    bool has_custom = false;
    for (const auto *decls : layers) {
        for (const auto &[key, val] : *decls) {
            if (key.size() >= 2 && key[0] == '-' && key[1] == '-') { has_custom = true; break; }
        }
        if (has_custom) break;
    }
    if (has_custom) {
        own_vars = *vars_ptr;  // the one copy, only for elements that redefine something
        for (const auto *decls : layers)
            for (const auto &[key, val] : *decls)
                if (key.size() >= 2 && key[0] == '-' && key[1] == '-') own_vars[key] = ResolveCssVars(val, own_vars);
        vars_ptr = &own_vars;
    }
    const std::unordered_map<std::string, std::string> &vars = *vars_ptr;

    for (const auto *decls : layers) {
        // A layer with no var() references applies as-is; only var()-using
        // values are resolved, into a small scratch overlay, sparing a map
        // rebuild per (node, matched rule).
        bool needs_resolve = false;
        for (const auto &[key, val] : *decls) {
            if ((key.size() < 2 || key[0] != '-' || key[1] != '-') && val.find("var(") != std::string::npos) {
                needs_resolve = true;
                break;
            }
        }
        bool has_custom_keys = false;
        for (const auto &[key, val] : *decls)
            if (key.size() >= 2 && key[0] == '-' && key[1] == '-') { has_custom_keys = true; break; }
        if (!needs_resolve && !has_custom_keys) {
            ApplyDeclarations(style, *decls);
            continue;
        }
        std::unordered_map<std::string, std::string> resolved;
        for (const auto &[key, val] : *decls) {
            if (key.size() >= 2 && key[0] == '-' && key[1] == '-') continue;  // custom property, not a real property
            resolved.emplace(key, val.find("var(") != std::string::npos ? ResolveCssVars(val, vars) : val);
        }
        ApplyDeclarations(style, resolved);
    }
}

// Whether a display-math span is the entirety of its parent block -- the
// shape `<p>$$..$$</p>` that org-mode's HTML export (and every other
// MathJax-targeting page) writes a displayed equation as. Whitespace-only
// text siblings do not count: the export routinely leaves a newline on
// either side of the span.
/**
 * @brief Checks whether `n` is its parent's only non-whitespace child.
 * @param n The node to test.
 * @return True when every sibling is whitespace-only text (or `n` has no parent).
 */
bool MathIsOnlyChild(const DomNode *n) {
    if (n->parent == nullptr) return true;
    for (const auto &sib : n->parent->children) {
        if (sib.get() == n) continue;
        if (sib->type == DomNodeType::Text) {
            if (sib->text.find_first_not_of(" \t\r\n") != std::string::npos) return false;
            continue;
        }
        return false;
    }
    return true;
}

/**
 * @brief Recursively computes and assigns the ComputedStyle for `n` and its descendants: tag defaults, inheritance from `parent`, matching CSS rules, then the inline style="" attribute, plus list-item nesting/marker bookkeeping.
 * @param n Node to style (no-op if it isn't an Element).
 * @param parent Already-computed style of `n`'s parent, used for inheritable properties.
 * @param rules Full list of collected CSS rules to match against `n`.
 * @param list_depth Current list nesting depth (0 = not inside a list) inherited from the caller.
 * @param in_ordered Whether the enclosing list (if any) is ordered (<ol>).
 */
void WalkAndStyle(DomNode *n, const ComputedStyle &parent, const std::vector<CssRule> &rules, int list_depth,
                   bool in_ordered, const std::unordered_map<std::string, std::string> &parent_vars) {
    if (n->type != DomNodeType::Element) return;
    // Custom properties inherit: this element reads its parent's set and only
    // gets a private copy (inside ApplyMatchingRules) if it redefines one.
    const std::unordered_map<std::string, std::string> *vars_ptr = &parent_vars;
    std::unordered_map<std::string, std::string> own_vars;
    ComputedStyle s = TagDefaults(n->tag);
    if (n->tag == "math") {
        bool display = n->attrs.count("display") && n->attrs.at("display") == "1";
        s.block = display;
        if (display) {
            // Air above and below, like every other block -- except when
            // the span is the whole of its parent paragraph, which is what
            // an org/MathJax export's `<p>$$..$$</p>` always is. There the
            // paragraph's own margins already provide it, and adding these
            // on top (this layout has no general margin collapsing) put
            // four blank lines around every displayed equation.
            const bool alone = MathIsOnlyChild(n);
            s.margin_top_lines = alone ? 0 : 1;
            s.margin_bottom_lines = alone ? 0 : 1;
        }
    }
    if (!s.has_color && parent.has_color) {
        s.has_color = true;
        s.color_r = parent.color_r;
        s.color_g = parent.color_g;
        s.color_b = parent.color_b;
    }
    // font-size inherits in real CSS (a <span>/<button> with no font-size
    // rule of its own renders at its *parent's* computed size, not the
    // root's) -- font_scale == 1.0f here means TagDefaults gave this tag
    // no distinctive size of its own (every tag except h1-h6), so it picks
    // up the cascaded parent value instead of resetting to the root size.
    // A heading's own fixed scale (TagDefaults, above) is left alone --
    // this renderer's `em` is root-relative, not chained parent-relative
    // (ApplyDeclarations' own font-size handling), so re-inheriting on top
    // of an already-distinctive heading scale would double-apply it.
    if (s.font_scale == 1.0f) s.font_scale = parent.font_scale;
    s.bold = s.bold || parent.bold;
    s.italic = s.italic || parent.italic;
    s.underline = s.underline || parent.underline;
    s.strikethrough = s.strikethrough || parent.strikethrough;
    s.monospace = s.monospace || parent.monospace;
    if (!s.monospace) s.font_family = parent.font_family;
    s.text_align = parent.text_align;
    s.line_height_multiplier = parent.line_height_multiplier;
    s.line_height_length = parent.line_height_length;
    s.letter_spacing = parent.letter_spacing;
    s.text_transform = parent.text_transform;  // inherits in real CSS (children opt out with `none`)
    if (n->tag != "pre") s.white_space = parent.white_space;
    // list-style-type inherits in real CSS: a `list-style: none` on the
    // <ul> reaches its <li> children (and deeper nested lists) unless one
    // re-specifies its own. Seeded from the parent before ApplyMatchingRules
    // (below) gets a chance to override it for this node.
    s.list_marker_none = parent.list_marker_none;
    s.list_depth = list_depth;
    s.visibility_hidden = parent.visibility_hidden;
    s.link_href = parent.link_href;
    s.link_node = parent.link_node;
    if (n->tag == "a") {
        if (auto it = n->attrs.find("href"); it != n->attrs.end()) {
            s.link_href = it->second;
            s.link_node = n;
        }
    }

    std::unordered_map<std::string, std::string> inline_decls;
    if (auto it = n->attrs.find("style"); it != n->attrs.end()) inline_decls = ParseDeclarations(it->second);
    ApplyMatchingRules(n, s, rules, inline_decls, vars_ptr, own_vars);
    const std::unordered_map<std::string, std::string> &vars = *vars_ptr;
    s.preserve_whitespace = s.white_space == HtmlWhiteSpace::Pre;
    s.faded = parent.faded || s.opacity_zero;
    // Flex items: a ROW-direction flex container's element children flow
    // inline on one line (the `.nav-row` toggle + label idiom); a column
    // container's children keep block stacking. Only the flow direction of
    // flex is approximated -- no grow/shrink/basis sizing. A container with a
    // SINGLE element child is the centering-wrapper idiom (a modal inside a
    // full-screen backdrop) -- that child must stay a block or its whole card
    // collapses into inline text.
    if (parent.flex_container && !parent.flex_column && !s.display_none && n->parent) {
        int element_kids = 0;
        for (const auto &sib : n->parent->children)
            if (sib->type == DomNodeType::Element) ++element_kids;
        if (element_kids >= 2) s.block = false;
    }
    n->style = s;

    bool is_list_container = n->tag == "ul" || n->tag == "ol";
    int next_depth = is_list_container ? list_depth + 1 : list_depth;
    bool next_ordered = is_list_container ? (n->tag == "ol") : in_ordered;
    int item_index = 0;
    for (auto &c : n->children) {
        if (is_list_container && c->type == DomNodeType::Element && c->tag == "li") item_index++;
        WalkAndStyle(c.get(), s, rules, next_depth, next_ordered, vars);
        if (is_list_container && c->type == DomNodeType::Element && c->tag == "li") {
            c->style.is_list_item = true;
            c->style.ordered_list_item = next_ordered;
            c->style.list_item_index = item_index;
        }
    }
    if (n->shadow_root) WalkAndStyle(n->shadow_root.get(), s, rules, next_depth, next_ordered, vars);
    if (n->tag == "select" && n->form_value.empty()) {
        DomNode *fallback = nullptr;
        for (auto &child : n->children) {
            if (child->type != DomNodeType::Element || child->tag != "option") continue;
            if (!fallback) fallback = child.get();
            if (child->attrs.count("selected") != 0) { fallback = child.get(); break; }
        }
        if (fallback) {
            auto value = fallback->attrs.find("value");
            if (value != fallback->attrs.end()) n->form_value = value->second;
            else for (const auto &text : fallback->children) if (text->type == DomNodeType::Text) n->form_value += text->text;
        }
    }
}

}  // namespace

bool CssEvalMediaQuery(const std::string &query) { return MediaQueryApplies(query); }

void ComputeStyles(HtmlDoc &doc) {
    if (!doc.root) return;
    // Rule cache: re-collecting + re-parsing every <style> block per cascade
    // dominated ComputeStyles once selector matching got cheap. The key
    // fingerprints each style text (length + endpoints) plus the @media
    // version, so script-injected styles and media flips rebuild it while the
    // hot path (a class toggle) reuses the parsed rules untouched.
    size_t key = 1469598103934665603ULL ^ (g_css_media_version * 1099511628211ULL);
    std::function<void(const DomNode *)> fingerprint = [&](const DomNode *n) {
        if (n->type == DomNodeType::Element && n->tag == "style") {
            for (const auto &c : n->children) {
                if (c->type != DomNodeType::Text) continue;
                key = (key ^ c->text.size()) * 1099511628211ULL;
                const size_t probe = std::min<size_t>(c->text.size(), 32);
                for (size_t i = 0; i < probe; ++i) key = (key ^ static_cast<unsigned char>(c->text[i])) * 1099511628211ULL;
                for (size_t i = c->text.size() >= 32 ? c->text.size() - 32 : 0; i < c->text.size(); ++i)
                    key = (key ^ static_cast<unsigned char>(c->text[i])) * 1099511628211ULL;
            }
        }
        for (const auto &c : n->children) fingerprint(c.get());
    };
    fingerprint(doc.root.get());
    auto cached = std::static_pointer_cast<std::vector<CssRule>>(doc.css_rules_cache);
    if (!cached || doc.css_rules_key != key) {
        cached = std::make_shared<std::vector<CssRule>>();
        CollectStyleRules(doc.root.get(), *cached);
        doc.css_rules_cache = cached;
        doc.css_rules_key = key;
    }
    const std::vector<CssRule> &rules = *cached;
    ComputedStyle root_style;  // no color/bold/italic -- layout falls back to the pane's theme colors
    const std::unordered_map<std::string, std::string> root_vars;  // custom properties cascade down from here
    for (auto &c : doc.root->children) WalkAndStyle(c.get(), root_style, rules, 0, false, root_vars);
}

std::vector<DomNode *> QuerySelectorAll(DomNode *root, const std::string &selector) {
    ParsedSelector parsed = ParseSelector(ToLower(selector));
    std::vector<DomNode *> matches;
    // Pseudo-element selectors (::before/::after) never match real elements
    // in querySelector -- they only exist for the cascade's generated content.
    if (root && parsed.valid && !parsed.pseudo_element) CollectSelectorMatches(root, root, parsed, matches);
    return matches;
}

DomNode *QuerySelector(DomNode *root, const std::string &selector) {
    std::vector<DomNode *> matches = QuerySelectorAll(root, selector);
    return matches.empty() ? nullptr : matches.front();
}

AccessibleNode BuildAccessibilityTree(const HtmlDoc &doc) {
    auto text_content = [](const DomNode *node, auto &&self) -> std::string {
        if (!node) return "";
        if (node->type == DomNodeType::Text) return node->text;
        std::string text;
        for (const auto &child : node->children) text += self(child.get(), self);
        return text;
    };
    auto role_for = [](const DomNode *node) {
        auto explicit_role = node->attrs.find("role");
        if (explicit_role != node->attrs.end()) return explicit_role->second;
        if (node->tag == "a") return std::string("link");
        if (node->tag == "button") return std::string("button");
        if (node->tag == "input") {
            auto type = node->attrs.find("type");
            if (type != node->attrs.end() && (type->second == "checkbox" || type->second == "radio")) return type->second;
            return std::string("textbox");
        }
        if (node->tag == "textarea") return std::string("textbox");
        if (node->tag == "select") return std::string("combobox");
        if (node->tag == "img") return std::string("img");
        if (node->tag == "main" || node->tag == "nav" || node->tag == "header" || node->tag == "footer") return node->tag;
        if (node->tag.size() == 2 && node->tag[0] == 'h' && std::isdigit(static_cast<unsigned char>(node->tag[1]))) return std::string("heading");
        if (node->tag == "ul" || node->tag == "ol") return std::string("list");
        if (node->tag == "li") return std::string("listitem");
        return std::string();
    };
    auto find_by_id = [](const DomNode *node, const std::string &id, auto &&self) -> const DomNode * {
        if (!node) return nullptr;
        if (node->Id() == id) return node;
        for (const auto &child : node->children) if (const DomNode *found = self(child.get(), id, self)) return found;
        if (node->shadow_root) if (const DomNode *found = self(node->shadow_root.get(), id, self)) return found;
        return nullptr;
    };
    // Appends `node`'s accessible children to `out_children`, mirroring how a
    // real browser's tree treats document structure: <html>/<body> are
    // transparent containers (their children surface directly -- so the
    // wrappers ParseHtml synthesizes around a body-less page never shift the
    // tree), and non-rendered subtrees (<head>, <style>, <script>, <title>)
    // are excluded entirely.
    std::function<void(const DomNode *, std::vector<AccessibleNode> &, const std::function<AccessibleNode(const DomNode *)> &)>
        append_children_impl = [&](const DomNode *node, std::vector<AccessibleNode> &out_children,
                                   const std::function<AccessibleNode(const DomNode *)> &build_one) {
            for (const auto &child : node->children) {
                if (child->type != DomNodeType::Element || child->style.display_none) continue;
                auto hidden = child->attrs.find("aria-hidden");
                if (hidden != child->attrs.end() && hidden->second == "true") continue;
                if (child->tag == "head" || child->tag == "style" || child->tag == "script" || child->tag == "title")
                    continue;
                if (child->tag == "html" || child->tag == "body") {
                    append_children_impl(child.get(), out_children, build_one);
                    continue;
                }
                out_children.push_back(build_one(child.get()));
            }
        };
    // The `-> void` is load-bearing, not decoration. `build` below calls
    // this, and this calls `build` back through `builder` -- so with a
    // deduced return type the compiler has to know this lambda's type
    // while it is still inside `build`'s own (not yet complete) body.
    // GCC and Clang let that pass; MSVC rejects it ("a function that
    // returns 'auto' cannot be used before it is defined"), and is right
    // to. Stating the type removes the need to deduce anything.
    auto append_children = [&](const DomNode *node, std::vector<AccessibleNode> &out_children,
                               auto &&builder) -> void {
        append_children_impl(node, out_children,
                             [&](const DomNode *child) { return builder(child, builder); });
    };
    auto build = [&](const DomNode *node, auto &&self) -> AccessibleNode {
        AccessibleNode accessible; accessible.role = role_for(node);
        auto label = node->attrs.find("aria-label");
        if (label != node->attrs.end()) accessible.name = label->second;
        else if (auto labelled_by = node->attrs.find("aria-labelledby"); labelled_by != node->attrs.end() && doc.root) {
            std::istringstream ids(labelled_by->second); std::string id;
            while (ids >> id) if (const DomNode *label_node = find_by_id(doc.root.get(), id, find_by_id)) {
                std::string part = text_content(label_node, text_content);
                if (!part.empty()) accessible.name += (accessible.name.empty() ? "" : " ") + part;
            }
        }
        else if (node->tag == "img" && node->attrs.count("alt")) accessible.name = node->attrs.at("alt");
        else accessible.name = text_content(node, text_content);
        if (auto description = node->attrs.find("aria-description"); description != node->attrs.end()) accessible.description = description->second;
        else if (auto described_by = node->attrs.find("aria-describedby"); described_by != node->attrs.end() && doc.root) {
            std::istringstream ids(described_by->second); std::string id;
            while (ids >> id) if (const DomNode *description_node = find_by_id(doc.root.get(), id, find_by_id)) {
                std::string part = text_content(description_node, text_content);
                if (!part.empty()) accessible.description += (accessible.description.empty() ? "" : " ") + part;
            }
        }
        accessible.disabled = node->form_disabled || node->attrs.count("aria-disabled") != 0;
        accessible.checked = node->form_checked || node->attrs.count("aria-checked") != 0;
        append_children(node, accessible.children, self);
        return accessible;
    };
    AccessibleNode root; root.role = "document"; root.name = doc.title;
    if (doc.root) append_children(doc.root.get(), root.children, build);
    return root;
}

bool CanvasGradientColorAt(const CanvasGradient &gradient, float x, float y,
                           unsigned char &r, unsigned char &g, unsigned char &b, unsigned char &a) {
    if (gradient.stops.empty()) return false;
    float t = 0.0f;
    if (gradient.radial) {
        float dx = x - gradient.x1, dy = y - gradient.y1;
        float span = gradient.r1 - gradient.r0;
        t = span <= 0.0f ? 1.0f : (std::sqrt(dx * dx + dy * dy) - gradient.r0) / span;
    } else {
        float ax = gradient.x1 - gradient.x0, ay = gradient.y1 - gradient.y0;
        float len2 = ax * ax + ay * ay;
        t = len2 <= 0.0f ? 0.0f : ((x - gradient.x0) * ax + (y - gradient.y0) * ay) / len2;
    }
    t = std::max(0.0f, std::min(1.0f, t));
    // Stops are kept sorted by offset at insertion (addColorStop).
    const CanvasGradientStop *lo = &gradient.stops.front(), *hi = &gradient.stops.back();
    for (size_t i = 0; i + 1 < gradient.stops.size(); ++i) {
        if (t >= gradient.stops[i].offset && t <= gradient.stops[i + 1].offset) { lo = &gradient.stops[i]; hi = &gradient.stops[i + 1]; break; }
    }
    if (t <= lo->offset) { r = lo->r; g = lo->g; b = lo->b; a = lo->a; return true; }
    if (t >= hi->offset) { r = hi->r; g = hi->g; b = hi->b; a = hi->a; return true; }
    float f = (t - lo->offset) / (hi->offset - lo->offset);
    auto mix = [f](unsigned char from, unsigned char to) { return static_cast<unsigned char>(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * f); };
    r = mix(lo->r, hi->r); g = mix(lo->g, hi->g); b = mix(lo->b, hi->b); a = mix(lo->a, hi->a);
    return true;
}

namespace {

void ForEachMediaNode(DomNode *node, const std::function<void(DomNode &)> &fn) {
    if (!node) return;
    if (node->type == DomNodeType::Element && (node->tag == "audio" || node->tag == "video")) fn(*node);
    for (auto &child : node->children) ForEachMediaNode(child.get(), fn);
    if (node->shadow_root) ForEachMediaNode(node->shadow_root.get(), fn);
}

}  // namespace

void LoadHtmlMedia(HtmlDoc &doc, const std::string &base_dir) {
    ForEachMediaNode(doc.root.get(), [&base_dir](DomNode &media) {
        media.media_duration = 0.0; media.media_ready_state = 0; media.media_error.clear(); media.media_source_path.clear();
        std::string src;
        if (auto it = media.attrs.find("src"); it != media.attrs.end()) src = it->second;
        if (src.empty())
            for (auto &child : media.children)
                if (child->type == DomNodeType::Element && child->tag == "source")
                    if (auto it = child->attrs.find("src"); it != child->attrs.end() && !it->second.empty()) { src = it->second; break; }
        if (src.empty()) return;
        if (src.compare(0, 7, "http://") == 0 || src.compare(0, 8, "https://") == 0) { media.media_error = "remote media sources are not fetched"; return; }
        std::string path = src[0] == '/' || base_dir.empty() ? src : base_dir + "/" + src;
        std::ifstream in(path, std::ios::binary);
        if (!in) { media.media_error = "MEDIA_ERR_SRC_NOT_SUPPORTED: cannot open " + src; return; }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        media.media_source_path = path;
        WavDoc wav;
        if (media.tag == "video" || !wav.LoadFromMemory(bytes.data(), bytes.size())) {
            media.media_error = "MEDIA_ERR_SRC_NOT_SUPPORTED: " + (media.tag == "video" ? std::string("no video decoder") : wav.Error());
            return;
        }
        media.media_duration = static_cast<double>(wav.Samples().size()) / static_cast<double>(wav.Channels()) / static_cast<double>(wav.SampleRate());
        media.media_ready_state = 4;  // HAVE_ENOUGH_DATA: the whole file is decoded
    });
}

void AdvanceHtmlMediaClock(HtmlDoc &doc, double seconds) {
    if (seconds <= 0.0) return;
    ForEachMediaNode(doc.root.get(), [seconds](DomNode &media) {
        if (media.media_paused || media.media_ready_state < 4 || media.media_duration <= 0.0) return;
        media.media_current_time += seconds;
        if (media.media_current_time < media.media_duration) return;
        if (media.attrs.count("loop")) { media.media_current_time = std::fmod(media.media_current_time, media.media_duration); return; }
        media.media_current_time = media.media_duration;
        media.media_paused = true;
        media.media_ended = true;
    });
}
