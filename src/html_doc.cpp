#include "html_doc.h"

#include <cctype>
#include <cstdlib>
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
            if (end != num_start && cp > 0 && cp < 128) {
                out += static_cast<char>(cp);
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

void ParseHtml(const std::string &html, HtmlDoc &out) {
    out.root = std::make_unique<DomNode>();
    out.root->type = DomNodeType::Element;
    out.root->tag = "#document";
    out.title.clear();
    out.scripts.clear();

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
        node->parent = stack.back();
        DomNode *raw = node.get();
        stack.back()->children.push_back(std::move(node));

        if (IsRawTextTag(tr.tag)) {
            size_t close_start = FindCloseTagCI(html, tr.tag, i);
            std::string raw_text = (close_start == std::string::npos) ? html.substr(i) : html.substr(i, close_start - i);
            if (tr.tag == "script") {
                out.scripts.push_back(raw_text);
            } else if (tr.tag == "style") {
                auto tnode = std::make_unique<DomNode>();
                tnode->type = DomNodeType::Text;
                tnode->text = raw_text;
                tnode->parent = raw;
                raw->children.push_back(std::move(tnode));
            }
            // textarea's raw content is intentionally dropped: this
            // renderer has no form/input model to hold it (see this
            // file's own header on out-of-scope features).
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

    // <title> is always near the document's start in practice, so a plain
    // breadth-first search (rather than a depth-first walk that might
    // detour deep into <body> first) finds it in the fewest node visits.
    std::vector<DomNode *> queue = {out.root.get()};
    while (!queue.empty() && out.title.empty()) {
        DomNode *cur = queue.front();
        queue.erase(queue.begin());
        if (cur->type == DomNodeType::Element && cur->tag == "title") {
            for (const auto &c : cur->children) {
                if (c->type == DomNodeType::Text) out.title += c->text;
            }
            break;
        }
        for (auto &c : cur->children) queue.push_back(c.get());
    }

    ExtractMathSpans(out);
    ComputeStyles(out);
}

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
    } else if (tag == "pre") {
        s.monospace = true;
        s.preserve_whitespace = true;
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag == "ul" || tag == "ol") {
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag == "hr") {
        s.margin_top_lines = 1;
        s.margin_bottom_lines = 1;
    } else if (tag == "head" || tag == "style" || tag == "script" || tag == "title" || tag == "meta" ||
               tag == "link" || tag == "#comment") {
        s.display_none = true;
    } else if (tag == "span" || tag == "small" || tag == "label" || tag == "td" || tag == "th" || tag == "br" ||
               tag == "img") {
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

struct CssRule {
    std::string selector;
    std::unordered_map<std::string, std::string> decls;
};

/**
 * @brief Parses a semicolon-separated "prop: value" declaration block into a property-name-to-value map, lowercasing and trimming both sides.
 * @param body Declaration block text (the contents between a CSS rule's braces, or an inline style="" attribute value).
 * @return Map of lowercased property names to lowercased, trimmed values.
 */
std::unordered_map<std::string, std::string> ParseDeclarations(const std::string &body) {
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
bool ParseColor(const std::string &raw, unsigned char *r, unsigned char *g, unsigned char *b) {
    std::string v = raw;
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
        if (auto it = decls.find(key); it != decls.end() && ParseColor(it->second, &r, &g, &b)) {
            s.has_bg = true;
            s.bg_r = r;
            s.bg_g = g;
            s.bg_b = b;
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
        if (it->second == "none") s.display_none = true;
        else if (it->second == "block" || it->second == "list-item")
            s.block = true;
        else if (it->second == "inline" || it->second == "inline-block")
            s.block = false;
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
}

/**
 * @brief Recursively collects CSS rules from every <style> element's text content in the subtree rooted at `n`, splitting comma-separated selector lists into individual rules.
 * @param n Root of the subtree to scan.
 * @param rules Output list appended with one CssRule per selector found.
 */
void CollectStyleRules(DomNode *n, std::vector<CssRule> &rules) {
    if (n->type == DomNodeType::Element && n->tag == "style") {
        std::string css;
        for (const auto &c : n->children) {
            if (c->type == DomNodeType::Text) css += c->text;
        }
        size_t i = 0, len = css.size();
        while (i < len) {
            size_t brace = css.find('{', i);
            if (brace == std::string::npos) break;
            size_t close = css.find('}', brace);
            if (close == std::string::npos) break;
            std::string selector_list = css.substr(i, brace - i);
            auto decls = ParseDeclarations(css.substr(brace + 1, close - brace - 1));
            size_t s = 0;
            while (s < selector_list.size()) {
                size_t comma = selector_list.find(',', s);
                std::string one = selector_list.substr(s, (comma == std::string::npos ? selector_list.size() : comma) - s);
                size_t a = one.find_first_not_of(" \t\r\n");
                size_t b = one.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) rules.push_back({ToLower(one.substr(a, b - a + 1)), decls});
                if (comma == std::string::npos) break;
                s = comma + 1;
            }
            i = close + 1;
        }
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
 * @brief Applies every CssRule whose selector matches node `n` in the given pass (tag selectors, or class/id selectors) to `style`.
 * @param n Node being styled; its tag/class/id are matched against each rule's selector.
 * @param style Style updated in place with the declarations of every matching rule, in rule order.
 * @param rules Full list of collected CSS rules to test against `n`.
 * @param tag_pass True to match only bare-tag selectors this pass; false to match only .class/#id selectors.
 */
void ApplyMatchingRules(const DomNode *n, ComputedStyle &style, const std::vector<CssRule> &rules, bool tag_pass) {
    for (const CssRule &r : rules) {
        if (r.selector.empty()) continue;
        bool matches = false;
        if (r.selector[0] == '.') matches = !tag_pass && n->Class() == r.selector.substr(1);
        else if (r.selector[0] == '#')
            matches = !tag_pass && n->Id() == r.selector.substr(1);
        else
            matches = tag_pass && n->tag == r.selector;
        if (matches) ApplyDeclarations(style, r.decls);
    }
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
                   bool in_ordered) {
    if (n->type != DomNodeType::Element) return;
    ComputedStyle s = TagDefaults(n->tag);
    if (n->tag == "math") {
        bool display = n->attrs.count("display") && n->attrs.at("display") == "1";
        s.block = display;
        if (display) {
            s.margin_top_lines = 1;
            s.margin_bottom_lines = 1;
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
    s.preserve_whitespace = s.preserve_whitespace || parent.preserve_whitespace;
    s.list_depth = list_depth;

    ApplyMatchingRules(n, s, rules, /*tag_pass=*/true);
    ApplyMatchingRules(n, s, rules, /*tag_pass=*/false);
    if (auto it = n->attrs.find("style"); it != n->attrs.end()) ApplyDeclarations(s, ParseDeclarations(it->second));
    n->style = s;

    bool is_list_container = n->tag == "ul" || n->tag == "ol";
    int next_depth = is_list_container ? list_depth + 1 : list_depth;
    bool next_ordered = is_list_container ? (n->tag == "ol") : in_ordered;
    int item_index = 0;
    for (auto &c : n->children) {
        if (is_list_container && c->type == DomNodeType::Element && c->tag == "li") item_index++;
        WalkAndStyle(c.get(), s, rules, next_depth, next_ordered);
        if (is_list_container && c->type == DomNodeType::Element && c->tag == "li") {
            c->style.is_list_item = true;
            c->style.ordered_list_item = next_ordered;
            c->style.list_item_index = item_index;
        }
    }
}

}  // namespace

void ComputeStyles(HtmlDoc &doc) {
    if (!doc.root) return;
    std::vector<CssRule> rules;
    CollectStyleRules(doc.root.get(), rules);
    ComputedStyle root_style;  // no color/bold/italic -- layout falls back to the pane's theme colors
    for (auto &c : doc.root->children) WalkAndStyle(c.get(), root_style, rules, 0, false);
}
