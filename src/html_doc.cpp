#include "html_doc.h"
#include "wav_doc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
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
            if (tr.tag == "script") {
                out.scripts.push_back(raw_text);
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
               tag == "img" || tag == "audio" || tag == "video" || tag == "input" || tag == "button" || tag == "select" || tag == "textarea" || tag == "option") {
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
    size_t source_order = 0;
};

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
                size_t op = body.find("~=");
                if (op != std::string::npos) attr.op = '~'; else { op = body.find('='); if (op != std::string::npos) attr.op = '='; }
                attr.name = ToLower(body.substr(0, op == std::string::npos ? body.size() : op));
                if (op != std::string::npos) {
                    size_t value_at = op + (attr.op == '~' ? 2 : 1);
                    attr.value = body.substr(value_at);
                    if (attr.value.size() >= 2 && (attr.value.front() == '\'' || attr.value.front() == '"') && attr.value.back() == attr.value.front()) attr.value = attr.value.substr(1, attr.value.size() - 2);
                }
                if (attr.name.empty()) return out;
                simple.attributes.push_back(std::move(attr)); out.class_count++; has_piece = true; i = close + 1;
            } else if (raw[i] == ':') {
                size_t name_start = ++i; while (i < raw.size() && IsCssIdentChar(raw[i])) ++i;
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
    out.valid = !out.parts.empty();
    return out;
}

bool HasClass(const DomNode *node, const std::string &want) {
    std::istringstream words(node->Class());
    std::string word;
    while (words >> word) if (word == want) return true;
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

bool MatchesSimple(const DomNode *node, const CssSimpleSelector &simple) {
    if (!node || node->type != DomNodeType::Element) return false;
    if (!simple.tag.empty() && node->tag != simple.tag) return false;
    if (!simple.id.empty() && node->Id() != simple.id) return false;
    for (const std::string &klass : simple.classes) if (!HasClass(node, klass)) return false;
    for (const auto &attr : simple.attributes) {
        auto it = node->attrs.find(attr.name); if (it == node->attrs.end()) return false;
        if (attr.op == '=' && it->second != attr.value) return false;
        if (attr.op == '~') {
            std::istringstream words(it->second); std::string word; bool found = false;
            while (words >> word) if (word == attr.value) { found = true; break; }
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
        // Interaction state is added with the event system; unsupported
        // pseudo classes never match rather than incorrectly styling all nodes.
        if (name == "hover" || name == "focus" || name == "active") return false;
    }
    return true;
}

bool MatchesSelectorAt(const DomNode *node, const ParsedSelector &selector, int part) {
    if (!MatchesSimple(node, selector.parts[static_cast<size_t>(part)])) return false;
    if (part == 0) return true;
    char combinator = selector.combinators[static_cast<size_t>(part - 1)];
    if (combinator == '>') return MatchesSelectorAt(node->parent, selector, part - 1);
    if (combinator == ' ') {
        for (const DomNode *ancestor = node->parent; ancestor; ancestor = ancestor->parent)
            if (MatchesSelectorAt(ancestor, selector, part - 1)) return true;
        return false;
    }
    if (!node->parent) return false;
    const auto &siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i) if (siblings[i].get() == node) {
        if (combinator == '+') {
            while (i > 0) { --i; if (siblings[i]->type == DomNodeType::Element) return MatchesSelectorAt(siblings[i].get(), selector, part - 1); }
            return false;
        }
        while (i > 0) { --i; if (siblings[i]->type == DomNodeType::Element && MatchesSelectorAt(siblings[i].get(), selector, part - 1)) return true; }
        return false;
    }
    return false;
}

void CollectSelectorMatches(DomNode *node, const ParsedSelector &selector, std::vector<DomNode *> &out) {
    if (!node) return;
    if (node->type == DomNodeType::Element && MatchesSelectorAt(node, selector, static_cast<int>(selector.parts.size()) - 1)) out.push_back(node);
    for (auto &child : node->children) CollectSelectorMatches(child.get(), selector, out);
}

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

bool ParseCssLength(const std::string &raw, CssLength &out, bool allow_auto = false) {
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
    char *end = nullptr;
    double value = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || value < 0.0) return false;
    std::string suffix = end;
    CssLength::Unit unit = CssLength::Unit::Px;
    if (suffix.empty() || suffix == "px") unit = CssLength::Unit::Px;
    else if (suffix == "%") unit = CssLength::Unit::Percent;
    else if (suffix == "em") unit = CssLength::Unit::Em;
    else if (suffix == "rem") unit = CssLength::Unit::Rem;
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
                if (a != std::string::npos) rules.push_back({ToLower(one.substr(a, b - a + 1)), decls, rules.size()});
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
 * @brief Applies every matching CSS rule in specificity/source order.
 */
void ApplyMatchingRules(const DomNode *n, ComputedStyle &style, const std::vector<CssRule> &rules) {
    struct Match { const CssRule *rule; ParsedSelector selector; };
    std::vector<Match> matches;
    for (const CssRule &r : rules) {
        ParsedSelector selector = ParseSelector(r.selector);
        if (selector.valid && MatchesSelectorAt(n, selector, static_cast<int>(selector.parts.size()) - 1))
            matches.push_back({&r, std::move(selector)});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.selector.id_count != b.selector.id_count) return a.selector.id_count < b.selector.id_count;
        if (a.selector.class_count != b.selector.class_count) return a.selector.class_count < b.selector.class_count;
        if (a.selector.tag_count != b.selector.tag_count) return a.selector.tag_count < b.selector.tag_count;
        return a.rule->source_order < b.rule->source_order;
    });
    for (const Match &match : matches) ApplyDeclarations(style, match.rule->decls);
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

    ApplyMatchingRules(n, s, rules);
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
    if (n->shadow_root) WalkAndStyle(n->shadow_root.get(), s, rules, next_depth, next_ordered);
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

void ComputeStyles(HtmlDoc &doc) {
    if (!doc.root) return;
    std::vector<CssRule> rules;
    CollectStyleRules(doc.root.get(), rules);
    ComputedStyle root_style;  // no color/bold/italic -- layout falls back to the pane's theme colors
    for (auto &c : doc.root->children) WalkAndStyle(c.get(), root_style, rules, 0, false);
}

std::vector<DomNode *> QuerySelectorAll(DomNode *root, const std::string &selector) {
    ParsedSelector parsed = ParseSelector(ToLower(selector));
    std::vector<DomNode *> matches;
    if (root && parsed.valid) CollectSelectorMatches(root, parsed, matches);
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
        for (const auto &child : node->children) {
            if (child->type != DomNodeType::Element || child->style.display_none) continue;
            auto hidden = child->attrs.find("aria-hidden");
            if (hidden != child->attrs.end() && hidden->second == "true") continue;
            accessible.children.push_back(self(child.get(), self));
        }
        return accessible;
    };
    AccessibleNode root; root.role = "document"; root.name = doc.title;
    if (doc.root) for (const auto &child : doc.root->children)
        if (child->type == DomNodeType::Element && !child->style.display_none &&
            !(child->attrs.count("aria-hidden") && child->attrs.at("aria-hidden") == "true")) root.children.push_back(build(child.get(), build));
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
