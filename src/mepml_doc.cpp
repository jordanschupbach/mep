#include "mepml_doc.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

namespace mepml {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }

int Len(const std::string &s) { return static_cast<int>(s.size()); }
char At(const std::string &s, int i) { return (i >= 0 && i < Len(s)) ? s[static_cast<size_t>(i)] : '\0'; }
std::string Sub(const std::string &s, int b, int e) {
    if (b < 0) b = 0;
    if (e > Len(s)) e = Len(s);
    if (e <= b) return std::string();
    return s.substr(static_cast<size_t>(b), static_cast<size_t>(e - b));
}
bool StartsAt(const std::string &s, int i, const char *lit) {
    for (int k = 0; lit[k]; ++k)
        if (At(s, i + k) != lit[k]) return false;
    return true;
}

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}
int Indent(const std::string &s) {
    int i = 0;
    while (i < Len(s) && (s[static_cast<size_t>(i)] == ' ' || s[static_cast<size_t>(i)] == '\t')) ++i;
    return i;
}
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads a `{...}` or `(...)` group starting at s[i] == '{' or '(',
// balancing nested braces (or parentheses) and honouring backslash escapes.
// Returns the index just past the closing brace (content is [i+1, ret-1)),
// or -1 if unbalanced before `limit`.
int ReadGroup(const std::string &s, int i, int limit) {
    const char open = At(s, i);
    if (open != '{' && open != '(') return -1;
    const char close = open == '{' ? '}' : ')';
    int depth = 0;
    for (int j = i; j < limit; ++j) {
        char c = s[static_cast<size_t>(j)];
        if (c == '\\') {
            ++j;
            continue;
        }
        if (c == open) ++depth;
        if (c == close && --depth == 0) return j + 1;
    }
    return -1;
}

// The top-level commas of the `(...)` group over [b, e) (its content, less
// the parentheses), at most `max` of them: `\fs(12, text, more)` splits
// once, so the text after the first comma may hold commas of its own.
std::vector<int> ArgCommas(const std::string &s, int b, int e, size_t max) {
    std::vector<int> out;
    int depth = 0;
    for (int j = b; j < e && out.size() < max; ++j) {
        char c = s[static_cast<size_t>(j)];
        if (c == '\\') {
            ++j;
            continue;
        }
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) out.push_back(j);
    }
    return out;
}

// Unescapes a `"quoted"` string's body.
std::string Unquote(const std::string &raw) {
    std::string t = Trim(raw);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        std::string out;
        for (size_t k = 1; k + 1 < t.size(); ++k) {
            if (t[k] == '\\' && k + 2 < t.size()) {
                ++k;
                out += t[k] == 'n' ? '\n' : t[k] == 't' ? '\t' : t[k];
            } else {
                out += t[k];
            }
        }
        return out;
    }
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// Value

Value Value::Parse(const std::string &raw) {
    Value v;
    std::string t = Trim(raw);
    bool quoted = t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front();
    v.s = Unquote(t);
    if (quoted || t.empty()) return v;
    const char *b = t.c_str();
    char *end = nullptr;
    errno = 0;
    long long iv = std::strtoll(b, &end, 10);
    if (errno == 0 && end && *end == '\0' && end != b) {
        v.kind = ValueKind::Int;
        v.i = iv;
        v.d = static_cast<double>(iv);
        return v;
    }
    errno = 0;
    double dv = std::strtod(b, &end);
    if (errno == 0 && end && *end == '\0' && end != b && t != "inf" && t != "nan") {
        v.kind = ValueKind::Double;
        v.d = dv;
        return v;
    }
    return v;
}

// Splits `a=1, b="x, y", c` on top-level commas (quotes respected).
static std::vector<std::string> SplitOptions(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        char c = s[k];
        if (quote) {
            cur += c;
            if (c == '\\' && k + 1 < s.size()) cur += s[++k];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur += c;
        } else if (c == ',') {
            out.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

static bool ParseAssignment(const std::string &s, Option *opt) {
    size_t eq = s.find('=');
    if (eq == std::string::npos) return false;
    opt->name = Trim(s.substr(0, eq));
    if (opt->name.empty()) return false;
    opt->value = Value::Parse(s.substr(eq + 1));
    return true;
}

// ---------------------------------------------------------------------------
// Block helpers

Block::Pos Block::OffsetToPos(int offset) const {
    Pos p;
    if (line_offsets.empty()) return p;
    auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), offset);
    int idx = static_cast<int>(it - line_offsets.begin()) - 1;
    if (idx < 0) idx = 0;
    p.line = line_start + idx;
    p.col = offset - line_offsets[static_cast<size_t>(idx)];
    return p;
}

const Option *Document::FindOption(const std::string &name) const {
    for (const Option &o : options)
        if (o.name == name) return &o;
    return nullptr;
}

int Document::BlockAtLine(int line) const {
    for (size_t k = 0; k < blocks.size(); ++k) {
        const Block &b = blocks[k];
        if (b.origin.empty() && line >= b.line_start && line <= b.line_end) return static_cast<int>(k);
    }
    return -1;
}

const std::vector<std::string> &CalloutKeywords() {
    static const std::vector<std::string> k = {"NOTE",   "WARNING", "ERROR",     "INFO", "TIP",
                                               "HINT",   "IMPORTANT", "CAUTION", "TODO", "FIXME",
                                               "DANGER", "SUCCESS", "QUESTION",  "EXAMPLE"};
    return k;
}

// ---------------------------------------------------------------------------
// Inline parser

namespace {

struct Marker {
    const char *open;
    const char *close;
    InlineKind kind;
    bool intraword;  // may open/close inside a word (super/subscript)
};

const Marker kMarkers[] = {
    {",,", ",,", InlineKind::Subscript, true}, {"*", "*", InlineKind::Bold, false},
    {"~", "~", InlineKind::Italic, false},     {"_", "_", InlineKind::Underline, false},
    {"^", "^", InlineKind::Superscript, true}, {"<", ">", InlineKind::Small, false},
    {">", "<", InlineKind::Big, false},        {"|", "|", InlineKind::Mono, false},
    {"=", "=", InlineKind::Highlight, false},  {"-", "-", InlineKind::Strike, false},
    {"+", "+", InlineKind::Insert, false},     {"!", "!", InlineKind::Delete, false},
};

bool IsMarkerChar(char c) { return c != '\0' && std::string("*~_^<>|=-+!,").find(c) != std::string::npos; }
bool PreOk(char c) {
    return c == '\0' || IsSpace(c) || c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' || c == '/' ||
           IsMarkerChar(c);
}
bool PostOk(char c) {
    return c == '\0' || IsSpace(c) || std::string(".,;:!?)]}\"'/").find(c) != std::string::npos || IsMarkerChar(c);
}

bool LooksLikeUrl(const std::string &s) {
    if (s.empty() || s.find(' ') != std::string::npos || s.find('\n') != std::string::npos) return false;
    return s.find("://") != std::string::npos || s.rfind("mailto:", 0) == 0 || s.rfind("file:", 0) == 0 ||
           s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0 || s.rfind("#", 0) == 0;
}

struct InlineCtx {
    const std::string &s;
    int *footnotes;
};

void ParseRange(InlineCtx &ctx, int b, int e, std::vector<Inline> &out);

// Finds the closing marker for an opener whose content starts at `from`.
int FindCloser(const std::string &s, int from, int e, const Marker &m) {
    int cl = static_cast<int>(std::char_traits<char>::length(m.close));
    for (int j = from; j + cl <= e; ++j) {
        char c = s[static_cast<size_t>(j)];
        if (c == '\\') {
            // A command's `(...)` arguments are inside it, like a `{...}` group.
            int k = j + 1;
            while (k < e && IsAlpha(s[static_cast<size_t>(k)])) ++k;
            const int g = k > j + 1 ? ReadGroup(s, k, e) : -1;
            j = g > 0 && At(s, k) == '(' ? g - 1 : j + 1;
            continue;
        }
        if (c == '`') {
            int k = static_cast<int>(s.find('`', static_cast<size_t>(j + 1)));
            if (k > 0 && k < e) j = k;
            continue;
        }
        if (c == '{') {
            int k = ReadGroup(s, j, e);
            if (k > 0) j = k - 1;
            continue;
        }
        if (!StartsAt(s, j, m.close) || j == from) continue;
        if (IsSpace(At(s, j - 1))) continue;
        if (!m.intraword && !(j + cl >= e ? true : PostOk(At(s, j + cl)))) continue;
        // `**`/`--` style doubled markers never close a single-char run.
        if (cl == 1 && At(s, j + 1) == m.close[0] && !m.intraword) continue;
        return j;
    }
    return -1;
}

bool TryMath(InlineCtx &ctx, int i, int e, Inline *node) {
    const std::string &s = ctx.s;
    int open = 0, close_at = -1, close_len = 0;
    bool display = false;
    if (StartsAt(s, i, "\\(")) {
        open = 2;
        size_t k = s.find("\\)", static_cast<size_t>(i + 2));
        if (k == std::string::npos || static_cast<int>(k) + 2 > e) return false;
        close_at = static_cast<int>(k);
        close_len = 2;
    } else if (StartsAt(s, i, "$$")) {
        open = 2;
        size_t k = s.find("$$", static_cast<size_t>(i + 2));
        if (k == std::string::npos || static_cast<int>(k) + 2 > e) return false;
        close_at = static_cast<int>(k);
        close_len = 2;
        display = true;
    } else if (At(s, i) == '$') {
        // Pandoc's rule: no space just inside either dollar, and a closing
        // dollar followed by a digit is a price, not math.
        if (IsSpace(At(s, i + 1)) || At(s, i + 1) == '\0') return false;
        open = 1;
        for (int j = i + 1; j < e; ++j) {
            char c = s[static_cast<size_t>(j)];
            if (c == '\\') {
                ++j;
                continue;
            }
            if (c == '$') {
                if (IsSpace(At(s, j - 1)) || IsDigit(At(s, j + 1))) return false;
                close_at = j;
                close_len = 1;
                break;
            }
        }
        if (close_at < 0) return false;
    } else {
        return false;
    }
    if (close_at <= i + open) return false;
    node->kind = InlineKind::Math;
    node->start = i;
    node->inner_start = i + open;
    node->inner_end = close_at;
    node->end = close_at + close_len;
    node->text = Sub(s, node->inner_start, node->inner_end);
    if (display) node->arg = "display";
    if (StartsAt(s, node->end, "\\alttext(") || StartsAt(s, node->end, "@alttext{")) {
        int g = ReadGroup(s, node->end + 8, e);
        if (g > 0) {
            node->alt = Sub(s, node->end + 9, g - 1);
            node->end = g;
        }
    }
    return true;
}

bool TryCommand(InlineCtx &ctx, int i, int e, Inline *node) {
    const std::string &s = ctx.s;
    int j = i + 1;
    while (j < e && IsAlpha(s[static_cast<size_t>(j)])) ++j;
    std::string name = Sub(s, i + 1, j);
    if (name.empty()) return false;
    int ngroups = 0;
    InlineKind kind;
    if (name == "f") kind = InlineKind::Font, ngroups = 2;
    else if (name == "fs") kind = InlineKind::FontSize, ngroups = 2;
    else if (name == "color") kind = InlineKind::Color, ngroups = 2;
    else if (name == "fn") kind = InlineKind::Footnote, ngroups = 1;
    else if (name == "cite") kind = InlineKind::Cite, ngroups = 1;
    else if (name == "citep") kind = InlineKind::CiteP, ngroups = 1;
    else return false;
    int g1 = ReadGroup(s, j, e);
    if (g1 < 0) return false;
    node->kind = kind;
    node->start = i;
    if (At(s, j) == '(') {
        // `\name(arg, text)`: the arguments before the text end at the
        // first top-level commas; the text is the rest.
        int from = j + 1;
        if (ngroups == 2) {
            const std::vector<int> commas = ArgCommas(s, j + 1, g1 - 1, 1);
            if (commas.empty()) return false;
            node->arg = Trim(Sub(s, j + 1, commas[0]));
            from = commas[0] + 1;
            while (from < g1 - 1 && IsSpace(s[static_cast<size_t>(from)])) ++from;
        }
        node->inner_start = from;
        node->inner_end = g1 - 1;
        node->end = g1;
    } else if (ngroups == 1) {
        node->inner_start = j + 1;
        node->inner_end = g1 - 1;
        node->end = g1;
    } else {
        int k = g1;
        while (k < e && IsSpace(s[static_cast<size_t>(k)])) ++k;
        int g2 = ReadGroup(s, k, e);
        if (g2 < 0) return false;
        node->arg = Trim(Sub(s, j + 1, g1 - 1));
        node->inner_start = k + 1;
        node->inner_end = g2 - 1;
        node->end = g2;
    }
    if (kind == InlineKind::Cite || kind == InlineKind::CiteP) {
        node->text = Trim(Sub(s, node->inner_start, node->inner_end));
    } else {
        if (kind == InlineKind::Footnote && ctx.footnotes) node->number = ++*ctx.footnotes;
        ParseRange(ctx, node->inner_start, node->inner_end, node->children);
    }
    return true;
}

bool TryLink(InlineCtx &ctx, int i, int e, Inline *node) {
    const std::string &s = ctx.s;
    int depth = 0, close = -1, bar = -1;
    for (int j = i; j < e; ++j) {
        char c = s[static_cast<size_t>(j)];
        if (c == '\\') {
            ++j;
            continue;
        }
        if (c == '\n') return false;
        if (c == '[') ++depth;
        if (c == ']' && --depth == 0) {
            close = j;
            break;
        }
        if (c == '|' && depth == 1) bar = j;  // last top-level bar: urls rarely contain one
    }
    if (close < 0) return false;
    node->kind = InlineKind::Link;
    node->start = i;
    node->end = close + 1;
    if (bar > 0) {
        node->arg = Trim(Sub(s, bar + 1, close));
        if (node->arg.empty()) return false;
        node->inner_start = i + 1;
        node->inner_end = bar;
        ParseRange(ctx, node->inner_start, node->inner_end, node->children);
    } else {
        std::string url = Sub(s, i + 1, close);
        if (!LooksLikeUrl(url)) return false;
        node->arg = url;
        node->inner_start = i + 1;
        node->inner_end = close;
        Inline t;
        t.kind = InlineKind::Text;
        t.text = url;
        t.start = t.inner_start = i + 1;
        t.end = t.inner_end = close;
        node->children.push_back(t);
    }
    return true;
}

bool TryEmphasis(InlineCtx &ctx, int i, int e, Inline *node) {
    const std::string &s = ctx.s;
    for (const Marker &m : kMarkers) {
        if (!StartsAt(s, i, m.open)) continue;
        int ol = static_cast<int>(std::char_traits<char>::length(m.open));
        char next = At(s, i + ol);
        if (i + ol >= e || IsSpace(next)) continue;
        if (ol == 1 && next == m.open[0] && !m.intraword) continue;  // `**`, `==`, `--`
        if (!m.intraword && !PreOk(At(s, i - 1))) continue;
        int j = FindCloser(s, i + ol, e, m);
        if (j < 0) continue;
        int cl = static_cast<int>(std::char_traits<char>::length(m.close));
        node->kind = m.kind;
        node->start = i;
        node->inner_start = i + ol;
        node->inner_end = j;
        node->end = j + cl;
        ParseRange(ctx, node->inner_start, node->inner_end, node->children);
        return true;
    }
    return false;
}

void ParseRange(InlineCtx &ctx, int b, int e, std::vector<Inline> &out) {
    const std::string &s = ctx.s;
    int text_start = b;
    auto flush = [&](int upto) {
        if (upto > text_start) {
            Inline t;
            t.kind = InlineKind::Text;
            t.text = Sub(s, text_start, upto);
            t.start = t.inner_start = text_start;
            t.end = t.inner_end = upto;
            out.push_back(std::move(t));
        }
    };
    int i = b;
    while (i < e) {
        char c = s[static_cast<size_t>(i)];
        Inline node;
        bool ok = false;
        if (c == '\\') {
            if (At(s, i + 1) == '(') ok = TryMath(ctx, i, e, &node);
            else if (IsAlpha(At(s, i + 1))) ok = TryCommand(ctx, i, e, &node);
            // An escape, or a `\(` that opens no maths: a literal parenthesis.
            if (!ok && !IsAlpha(At(s, i + 1)) && i + 1 < e && !IsSpace(At(s, i + 1))) {
                // Escape: `\*` is a literal star. The backslash is markup.
                flush(i);
                Inline t;
                t.kind = InlineKind::Text;
                t.text = Sub(s, i + 1, i + 2);
                t.start = i;
                t.inner_start = i + 1;
                t.end = t.inner_end = i + 2;
                out.push_back(std::move(t));
                i += 2;
                text_start = i;
                continue;
            }
        } else if (c == '`') {
            size_t k = s.find('`', static_cast<size_t>(i + 1));
            if (k != std::string::npos && static_cast<int>(k) < e && static_cast<int>(k) > i + 1) {
                node.kind = InlineKind::Verbatim;
                node.start = i;
                node.inner_start = i + 1;
                node.inner_end = static_cast<int>(k);
                node.end = static_cast<int>(k) + 1;
                node.text = Sub(s, node.inner_start, node.inner_end);
                ok = true;
            }
        } else if (c == '$') {
            ok = TryMath(ctx, i, e, &node);
        } else if (c == '[') {
            ok = TryLink(ctx, i, e, &node);
        } else if (c == '/' && At(s, i + 1) == '/' && (i == 0 || IsSpace(At(s, i - 1))) &&
                   (IsSpace(At(s, i + 2)) || At(s, i + 2) == '\0' || i + 2 >= e)) {
            int k = i;
            while (k < e && s[static_cast<size_t>(k)] != '\n') ++k;
            node.kind = InlineKind::Comment;
            node.start = i;
            node.inner_start = i + 2;
            node.inner_end = node.end = k;
            node.text = Trim(Sub(s, i + 2, k));
            ok = true;
        } else if (IsMarkerChar(c)) {
            ok = TryEmphasis(ctx, i, e, &node);
        }
        if (ok) {
            flush(i);
            i = node.end;
            out.push_back(std::move(node));
            text_start = i;
        } else {
            ++i;
        }
    }
    flush(e);
}

}  // namespace

std::vector<Inline> ParseInlines(const std::string &text, int *footnote_counter) {
    std::vector<Inline> out;
    InlineCtx ctx{text, footnote_counter};
    ParseRange(ctx, 0, Len(text), out);
    return out;
}

// ---------------------------------------------------------------------------
// Block parser

namespace {

bool IsBibtexType(const std::string &name) {
    static const std::set<std::string> k = {"article",   "book",          "booklet",      "conference",
                                            "inbook",    "incollection",  "inproceedings", "manual",
                                            "mastersthesis", "misc",      "phdthesis",    "proceedings",
                                            "techreport", "unpublished",  "online",       "software"};
    return k.count(Lower(name)) > 0;
}

int HeadingLevel(const std::string &line) {
    int n = 0;
    while (n < Len(line) && line[static_cast<size_t>(n)] == '>') ++n;
    if (n == 0 || n > 6) return 0;
    if (n == Len(line)) return n;  // an empty heading still reads as one
    return line[static_cast<size_t>(n)] == ' ' || line[static_cast<size_t>(n)] == '\t' ? n : 0;
}

bool IsRule(const std::string &t) {
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '=' && c != '_' && c != '*') return false;
    for (char d : t)
        if (d != c) return false;
    return true;
}

bool IsFence(const std::string &t) { return t.rfind("```", 0) == 0; }

bool IsTableRow(const std::string &t) { return t.size() >= 2 && t.front() == '|' && t.back() == '|'; }

// A `|---|:--:|` row: at least one pipe, and every cell dashes (or `=`)
// with optional alignment colons.
bool IsDelimiterRow(const std::string &s, size_t *cells_out = nullptr) {
    const std::vector<std::pair<int, int>> cells = TableCells(s);
    if (cells.empty()) return false;
    for (const auto &cell : cells) {
        std::string t = Trim(Sub(s, cell.first, cell.second));
        if (t.empty()) return false;
        for (char ch : t)
            if (ch != '-' && ch != ':' && ch != '=') return false;
    }
    if (cells_out) *cells_out = cells.size();
    return true;
}

// A list item's marker: "- ", "* ", "+ ", "12. ", "3) ". Returns the byte
// length of indent+marker+space, 0 if not an item.
int ListMarker(const std::string &line, bool *ordered, int *number) {
    int i = Indent(line);
    char c = At(line, i);
    int j = i;
    if (c == '-' || c == '*' || c == '+') {
        j = i + 1;
        *ordered = false;
    } else if (IsDigit(c)) {
        while (IsDigit(At(line, j))) ++j;
        if (j - i > 9 || (At(line, j) != '.' && At(line, j) != ')')) return 0;
        *number = std::atoi(Sub(line, i, j).c_str());
        *ordered = true;
        ++j;
    } else {
        return 0;
    }
    if (At(line, j) != ' ' && At(line, j) != '\t') return 0;
    while (At(line, j) == ' ' || At(line, j) == '\t') ++j;
    return j;
}

bool IsListItem(const std::string &line) {
    bool o = false;
    int n = 0;
    return !IsRule(Trim(line)) && ListMarker(line, &o, &n) > 0;
}

// "// KEYWORD:" -> KEYWORD, else "".
std::string CalloutKeyword(const std::string &line) {
    std::string t = Trim(line);
    if (t.rfind("//", 0) != 0 || t.rfind("//?", 0) == 0) return "";
    size_t k = 2;
    while (k < t.size() && t[k] == ' ') ++k;
    size_t w = k;
    while (w < t.size() && IsUpper(t[w])) ++w;
    if (w == k || w >= t.size() || t[w] != ':') return "";
    std::string kw = t.substr(k, w - k);
    const auto &kws = CalloutKeywords();
    return std::find(kws.begin(), kws.end(), kw) != kws.end() ? kw : "";
}

bool IsComment(const std::string &line) { return Trim(line).rfind("//", 0) == 0; }
bool IsMetaLine(const std::string &line) { return Trim(line).rfind("//?", 0) == 0; }
// `// result_begin:` optionally followed by the results' kind (`html`,
// `markdown`; `md` is read as `markdown`).
bool IsResultBegin(const std::string &line, std::string *format = nullptr) {
    std::string t = Trim(line);
    std::string rest;
    if (t.rfind("// result_begin", 0) == 0) rest = t.substr(15);
    else if (t.rfind("//result_begin", 0) == 0) rest = t.substr(14);
    else return false;
    if (!rest.empty() && rest[0] == ':') rest = rest.substr(1);
    rest = Trim(rest);
    for (char c : rest)
        if (!IsAlpha(c)) return false;
    rest = Lower(rest);
    if (rest == "md") rest = "markdown";
    if (format) *format = rest;
    return true;
}
bool IsResultEnd(const std::string &line) {
    std::string t = Trim(line);
    return t == "// result_end" || t == "// result_end:" || t == "//result_end";
}

bool IsKnownDirective(const std::string &name) {
    return name == "import" || name == "citation" || name == "image" || name == "caption" || name == "alttext" ||
           name == "bibliography" || name == "printbibliography" || name == "toc" || name == "abstract" ||
           IsBibtexType(name);
}

// `\slide(` (or `\slide{`, `@slide{`) at the start of a line: the column
// just past its bracket, with *close the bracket that ends the slide; -1
// for any other line.
int SlideOpener(const std::string &line, char *close = nullptr) {
    const int i = Indent(line);
    const char c = At(line, i);
    if ((c != '\\' && c != '@') || !StartsAt(line, i + 1, "slide")) return -1;
    const char b = At(line, i + 6);
    if (b != '{' && (b != '(' || c != '\\')) return -1;
    if (close) *close = b == '(' ? ')' : '}';
    return i + 7;
}
// A line that ends a slide opened with `close`: that bracket on its own,
// perhaps with a `// comment` after it.
bool IsSlideCloser(const std::string &line, char close) {
    const std::string t = Trim(line);
    if (close == 0 || t.empty() || t[0] != close) return false;
    const std::string rest = Trim(t.substr(1));
    return rest.empty() || rest.rfind("//", 0) == 0;
}

// `\name(...)` (or the older `@name{...}`) at the start of a trimmed line;
// *sigil gets the `\` or `@`. A backslash starts a directive only for a
// directive's own name followed by its `(` (or nothing, `\toc`), so a
// paragraph may start with an inline command; BibTeX entries are `@` only.
std::string DirectiveName(const std::string &line, char *sigil = nullptr) {
    int i = Indent(line);
    const char c = At(line, i);
    if (c != '@' && c != '\\') return "";
    int j = i + 1;
    while (IsAlpha(At(line, j))) ++j;
    std::string name = Sub(line, i + 1, j);
    if (c == '\\') {
        const char next = At(line, j);
        if (!IsKnownDirective(name) || IsBibtexType(name) || name == "printbibliography" ||
            (next != '(' && next != '\0' && !IsSpace(next)))
            return "";
    }
    if (sigil) *sigil = c;
    return name;
}

// Parses "k = v, k = {v}, k = "v"" fields out of a citation body.
void ParseFields(const std::string &body, std::map<std::string, std::string> *fields,
                 std::vector<std::string> *order) {
    size_t k = 0;
    while (k < body.size()) {
        while (k < body.size() && (IsSpace(body[k]) || body[k] == ',')) ++k;
        size_t name_b = k;
        while (k < body.size() && body[k] != '=' && body[k] != ',' && body[k] != '\n') ++k;
        if (k >= body.size() || body[k] != '=') {
            // Not a field; skip the rest of this line.
            while (k < body.size() && body[k] != '\n') ++k;
            continue;
        }
        std::string name = Lower(Trim(body.substr(name_b, k - name_b)));
        ++k;
        while (k < body.size() && (body[k] == ' ' || body[k] == '\t')) ++k;
        std::string value;
        if (k < body.size() && body[k] == '{') {
            int g = ReadGroup(body, static_cast<int>(k), Len(body));
            if (g < 0) g = Len(body);
            value = body.substr(k + 1, static_cast<size_t>(g - 1) - (k + 1));
            k = static_cast<size_t>(g);
        } else if (k < body.size() && body[k] == '"') {
            size_t e = k + 1;
            while (e < body.size() && body[e] != '"') e += body[e] == '\\' ? 2 : 1;
            value = body.substr(k + 1, std::min(e, body.size()) - (k + 1));
            k = e + 1;
        } else {
            size_t e = k;
            while (e < body.size() && body[e] != ',' && body[e] != '\n') ++e;
            value = Trim(body.substr(k, e - k));
            k = e;
        }
        // Collapse inner whitespace/newlines so multi-line titles read as one.
        std::string flat;
        bool sp = false;
        for (char c : value) {
            if (IsSpace(c)) {
                sp = true;
                continue;
            }
            if (sp && !flat.empty()) flat += ' ';
            sp = false;
            if (c != '{' && c != '}') flat += c;
        }
        if (!name.empty()) {
            if (!fields->count(name)) order->push_back(name);
            (*fields)[name] = flat;
        }
    }
}

struct Parser {
    const std::vector<std::string> &lines;
    Document doc;
    int footnotes = 0;
    int n = 0;
    // The `// result_end` lines closing `results=markdown` regions. Their
    // content is parsed as blocks of the document, so the closing marker
    // is skipped rather than read as a comment, and a caption under it
    // still reaches the table (or other block) the results end with.
    std::set<int> markdown_result_ends;

    explicit Parser(const std::vector<std::string> &l) : lines(l), n(static_cast<int>(l.size())) {}

    const std::string &L(int i) const { return lines[static_cast<size_t>(i)]; }

    void Diag(Diagnostic::Severity sev, int line, int cb, int ce, const std::string &msg) {
        Diagnostic d;
        d.severity = sev;
        d.line = line;
        d.col_start = cb;
        d.col_end = ce;
        d.message = msg;
        doc.diagnostics.push_back(std::move(d));
    }

    Block MakeBlock(BlockKind kind, int first, int last) {
        Block b;
        b.kind = kind;
        b.line_start = first;
        b.line_end = last;
        SetText(b);
        return b;
    }
    void SetText(Block &b) {
        b.text.clear();
        b.line_offsets.clear();
        for (int k = b.line_start; k <= b.line_end; ++k) {
            if (k > b.line_start) b.text += '\n';
            b.line_offsets.push_back(Len(b.text));
            b.text += L(k);
        }
    }
    std::vector<Inline> Inlines(const Block &b, int from, int to) {
        std::vector<Inline> out;
        InlineCtx ctx{b.text, &footnotes};
        ParseRange(ctx, from, to, out);
        return out;
    }

    // How a directive is written, for messages: `\image(...)`, `\image(...)`.
    static std::string Form(char sigil, const std::string &name) {
        return sigil == '@' ? "@" + name + "{...}" : "\\" + name + "(...)";
    }

    // Text after a directive's groups must be empty or a `// comment`.
    void CheckTrailing(int line, int col) {
        std::string rest = Trim(Sub(L(line), col, Len(L(line))));
        if (!rest.empty() && rest.rfind("//", 0) != 0)
            Diag(Diagnostic::Warning, line, col, Len(L(line)), "unexpected text after directive");
    }

    // A directive group on one line: `@name{arg}`. Returns arg; `*after`
    // is the column past the closing brace, -1 when unbalanced.
    std::string OneLineGroup(int line, int col, int *after) {
        const std::string &s = L(line);
        int g = ReadGroup(s, col, Len(s));
        *after = g;
        if (g < 0) return "";
        return Sub(s, col + 1, g - 1);
    }

    // A directive group which may run over several lines: `@name{` on line
    // `line` at column `col`, closed by its balancing `}` on that line or
    // any later one before a blank line. Returns the closing line, with
    // *after the column past its `}`; -1 when it never closes.
    int MultiLineGroup(int line, int col, int *after) {
        std::string joined;
        std::vector<int> starts;
        for (int k = line; k < n; ++k) {
            if (k > line && Trim(L(k)).empty()) break;
            if (k > line) joined += '\n';
            starts.push_back(Len(joined));
            joined += L(k);
            const int g = ReadGroup(joined, col, Len(joined));
            if (g > 0) {
                *after = g - starts.back();
                return k;
            }
        }
        *after = -1;
        return -1;
    }

    // Line breaks (and the indentation after them) in a caption's text
    // runs read as one space, as they would in a paragraph.
    static std::string OneSpaced(const std::string &t) {
        std::string o;
        bool space = false;
        for (char c : t) {
            if (c == '\n' || c == '\r' || ((c == ' ' || c == '\t') && space)) {
                if (!space) o += ' ';
                space = true;
                continue;
            }
            space = c == ' ' || c == '\t';
            o += c;
        }
        return o;
    }
    static void OneSpacedInlines(std::vector<Inline> &ins) {
        for (Inline &x : ins) {
            if (x.kind == InlineKind::Text) x.text = OneSpaced(x.text);
            OneSpacedInlines(x.children);
        }
    }

    // Blocks a caption/alttext line may attach to. Returns the last line
    // the directive covers: -1 when there is nothing to attach to, -2 when
    // its group never closes (reported here).
    int Attach(int i, const std::string &name, char sigil) {
        if (doc.blocks.empty()) return -1;
        Block &b = doc.blocks.back();
        const int prev = markdown_result_ends.count(i - 1) ? i - 2 : i - 1;
        if (b.line_end != prev) return -1;
        if (b.kind != BlockKind::Image && b.kind != BlockKind::Table && b.kind != BlockKind::MathBlock &&
            b.kind != BlockKind::Code)
            return -1;
        int col = Indent(L(i)) + 1 + Len(name);
        int after = -1;
        const int last = MultiLineGroup(i, col, &after);
        if (last < 0) {
            Diag(Diagnostic::Error, i, 0, Len(L(i)), "unterminated " + Form(sigil, name));
            return -2;
        }
        CheckTrailing(last, after);
        b.line_end = last;
        SetText(b);
        // The group's content, less the blanks (and line breaks) just
        // inside its braces.
        int from = b.line_offsets[static_cast<size_t>(i - b.line_start)] + col + 1;
        int to = b.line_offsets[static_cast<size_t>(last - b.line_start)] + after - 1;
        while (from < to && std::isspace(static_cast<unsigned char>(b.text[static_cast<size_t>(from)]))) ++from;
        while (to > from && std::isspace(static_cast<unsigned char>(b.text[static_cast<size_t>(to - 1)]))) --to;
        const std::string arg = OneSpaced(Sub(b.text, from, to));
        if (name == "caption") {
            b.caption = arg;
            b.caption_line = i;
            b.caption_line_end = last;
            b.caption_close_col = after - 1;
            b.caption_inlines = Inlines(b, from, to);
            OneSpacedInlines(b.caption_inlines);
        } else {
            b.alt = arg;
            b.alt_line = i;
            b.alt_line_end = last;
            b.alt_close_col = after - 1;
        }
        return last;
    }

    int ParseMetaRun(int i) {
        int j = i;
        while (j < n && IsMetaLine(L(j))) ++j;
        // Directly followed by a code fence: the options belong to it.
        if (j < n && IsFence(Trim(L(j)))) return ParseCode(j, i);
        for (int k = i; k < j; ++k) {
            Block b = MakeBlock(BlockKind::Meta, k, k);
            std::string body = Trim(Trim(L(k)).substr(3));
            size_t colon = body.find(':');
            if (colon == std::string::npos) {
                b.keyword = body;
            } else {
                b.keyword = Trim(body.substr(0, colon));
                b.value = Trim(body.substr(colon + 1));
            }
            doc.meta.emplace_back(b.keyword, b.value);
            std::string key = Lower(b.keyword);
            if (key == "title") {
                doc.title = b.value;
            } else if (key == "import") {
                if (Trim(b.value).empty()) Diag(Diagnostic::Error, k, 0, Len(L(k)), "Import: needs a path");
            } else if (key == "option") {
                Option o;
                if (ParseAssignment(b.value, &o)) {
                    o.line = k;
                    b.options.push_back(o);
                    doc.options.push_back(o);
                } else {
                    Diag(Diagnostic::Warning, k, 0, Len(L(k)), "Option: expects Name=value");
                }
            }
            doc.blocks.push_back(std::move(b));
        }
        return j;
    }

    int ParseCode(int fence, int opts_from) {
        std::string head = Trim(L(fence)).substr(3);
        Block b;
        b.kind = BlockKind::Code;
        for (int k = opts_from; k < fence; ++k) {
            std::string body = Trim(Trim(L(k)).substr(3));
            size_t colon = body.find(':');
            std::string key = colon == std::string::npos ? body : Trim(body.substr(0, colon));
            std::string val = colon == std::string::npos ? "" : Trim(body.substr(colon + 1));
            Option o;
            o.line = k;
            if (Lower(key) == "option") {
                if (!ParseAssignment(val, &o)) {
                    Diag(Diagnostic::Warning, k, 0, Len(L(k)), "Option: expects Name=value");
                    continue;
                }
            } else {
                o.name = key;
                o.value = Value::Parse(val);
            }
            b.options.push_back(o);
        }
        // Header: {lang, k=v, ...} | lang | lang {k=v}
        std::string h = Trim(head);
        std::string opts;
        if (!h.empty() && h.front() == '{') {
            size_t close = h.rfind('}');
            opts = h.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        } else {
            size_t brace = h.find('{');
            b.lang = Trim(h.substr(0, brace));
            if (brace != std::string::npos) {
                size_t close = h.rfind('}');
                opts = h.substr(brace + 1, close == std::string::npos || close < brace ? std::string::npos
                                                                                         : close - brace - 1);
            }
        }
        if (!opts.empty() || (!h.empty() && h.front() == '{')) {
            std::vector<std::string> parts = SplitOptions(opts);
            for (size_t k = 0; k < parts.size(); ++k) {
                Option o;
                o.line = fence;
                if (ParseAssignment(parts[k], &o)) {
                    b.options.push_back(o);
                } else if (k == 0 && b.lang.empty()) {
                    b.lang = parts[k];
                } else if (!parts[k].empty()) {
                    // A bare flag: `{python, eval}` == eval=true.
                    o.name = parts[k];
                    o.value = Value::Parse("true");
                    b.options.push_back(o);
                }
            }
        }
        int j = fence + 1;
        while (j < n && Trim(L(j)) != "```") ++j;
        b.code_line_start = fence + 1;
        b.code_line_end = j - 1;
        for (int k = fence + 1; k < j && k < n; ++k) {
            if (k > fence + 1) b.code += '\n';
            b.code += L(k);
        }
        if (j >= n) {
            Diag(Diagnostic::Error, fence, 0, Len(L(fence)), "code block is never closed with ```");
            j = n - 1;
        }
        int last = j;
        if (j + 1 < n && IsResultBegin(L(j + 1), &b.result_format) && b.result_format == "markdown") {
            // Markdown the block printed (`results=markdown`): written out
            // raw, not commented, and read as part of the document -- a
            // table it printed is a table here, numbered and captioned like
            // one typed in. The block owns only the markers; what lies
            // between them is left for Run() to parse.
            int r = j + 2;
            while (r < n && !IsResultEnd(L(r)) && !IsFence(Trim(L(r))) && !HeadingLevel(L(r))) ++r;
            b.result_line_start = j + 1;
            if (r < n && IsResultEnd(L(r))) {
                b.result_line_end = r;
                markdown_result_ends.insert(r);
            } else {
                Diag(Diagnostic::Error, j + 1, 0, Len(L(j + 1)), "results region has no // result_end");
                b.result_line_end = r - 1;
            }
            for (int k = j + 2; k < r; ++k) b.result_lines.push_back(L(k));
            b.line_start = opts_from;
            b.line_end = j + 1;
            SetText(b);
            doc.blocks.push_back(std::move(b));
            return j + 2;
        }
        if (j + 1 < n && IsResultBegin(L(j + 1), &b.result_format)) {
            int r = j + 2;
            while (r < n && !IsResultEnd(L(r)) && IsComment(L(r))) ++r;
            b.result_line_start = j + 1;
            if (r < n && IsResultEnd(L(r))) {
                b.result_line_end = r;
            } else {
                Diag(Diagnostic::Error, j + 1, 0, Len(L(j + 1)), "results region has no // result_end");
                b.result_line_end = r - 1;
            }
            for (int k = j + 2; k < b.result_line_end || (k == b.result_line_end && !IsResultEnd(L(k))); ++k) {
                std::string t = L(k);
                size_t p = t.find("//");
                t = p == std::string::npos ? t : t.substr(p + 2);
                if (!t.empty() && t[0] == ' ') t.erase(0, 1);
                std::string img;
                if (b.result_format.empty() && ResultImagePath(t, &img)) b.result_images.emplace_back(k, img);
                b.result_lines.push_back(t);
            }
            last = b.result_line_end;
        }
        b.line_start = opts_from;
        b.line_end = last;
        SetText(b);
        doc.blocks.push_back(std::move(b));
        return last + 1;
    }

    int ParseComment(int i) {
        std::string kw = CalloutKeyword(L(i));
        int j = i + 1;
        if (!kw.empty()) {
            while (j < n && IsComment(L(j)) && !IsMetaLine(L(j)) && CalloutKeyword(L(j)).empty() &&
                   !IsResultBegin(L(j)))
                ++j;
            Block b = MakeBlock(BlockKind::Callout, i, j - 1);
            b.keyword = kw;
            // Body: first line after "KEYWORD:", continuation lines after "//".
            for (int k = i; k < j; ++k) {
                int off = b.line_offsets[static_cast<size_t>(k - i)];
                const std::string &s = L(k);
                int c = static_cast<int>(s.find("//")) + 2;
                if (k == i) c = static_cast<int>(s.find(':', static_cast<size_t>(c))) + 1;
                std::vector<Inline> part = Inlines(b, off + c, off + Len(s));
                if (k > i && !b.inlines.empty()) {
                    Inline sp;
                    sp.kind = InlineKind::Text;
                    sp.text = " ";
                    sp.start = sp.inner_start = off - 1;
                    sp.end = sp.inner_end = off;
                    b.inlines.push_back(sp);
                }
                for (Inline &x : part) b.inlines.push_back(std::move(x));
            }
            doc.blocks.push_back(std::move(b));
            return j;
        }
        if (IsResultBegin(L(i)) || IsResultEnd(L(i)))
            Diag(Diagnostic::Warning, i, 0, Len(L(i)), "results marker not attached to a code block");
        while (j < n && IsComment(L(j)) && !IsMetaLine(L(j)) && CalloutKeyword(L(j)).empty()) ++j;
        doc.blocks.push_back(MakeBlock(BlockKind::Comment, i, j - 1));
        return j;
    }

    int ParseDisplayMath(int i) {
        std::string t = Trim(L(i));
        bool dollars = t.rfind("$$", 0) == 0;
        const char *close = dollars ? "$$" : "\\]";
        int ind = Indent(L(i));
        int j = i;
        size_t found = L(i).find(close, static_cast<size_t>(ind + 2));
        if (found == std::string::npos) {
            for (j = i + 1; j < n; ++j) {
                found = L(j).find(close);
                if (found != std::string::npos) break;
            }
        }
        if (j >= n) {
            Diag(Diagnostic::Error, i, 0, Len(L(i)), std::string("display math is never closed with ") + close);
            j = n - 1;
        }
        Block b = MakeBlock(BlockKind::MathBlock, i, j);
        int from = b.line_offsets[0] + ind + 2;
        int to = found == std::string::npos ? Len(b.text)
                                            : b.line_offsets[static_cast<size_t>(j - i)] + static_cast<int>(found);
        b.code = Trim(Sub(b.text, from, to));
        if (found != std::string::npos) CheckTrailing(j, static_cast<int>(found) + 2);
        doc.blocks.push_back(std::move(b));
        return j + 1;
    }

    int ParseDirective(int i, const std::string &name, char sigil) {
        const std::string &s = L(i);
        int col = Indent(s) + 1 + Len(name);
        if (name == "caption" || name == "alttext") {
            const int last = Attach(i, name, sigil);
            if (last < 0) {
                if (last == -1)
                    Diag(Diagnostic::Warning, i, 0, Len(s),
                         std::string(1, sigil) + name + " must directly follow an image, table, code block or display math");
                int after = -1;
                const int end = std::max(i, MultiLineGroup(i, col, &after));
                doc.blocks.push_back(MakeBlock(BlockKind::Comment, i, end));
                return end + 1;
            }
            return last + 1;
        }
        if (name == "printbibliography")
            Diag(Diagnostic::Info, i, Indent(s), col, "@printbibliography is now \\bibliography");
        if (name == "bibliography" || name == "printbibliography" || name == "toc") {
            CheckTrailing(i, col);
            doc.blocks.push_back(
                MakeBlock(name == "toc" ? BlockKind::TableOfContents : BlockKind::Bibliography, i, i));
            return i + 1;
        }
        if (name == "import" || name == "image") {
            int after = -1;
            std::string arg = OneLineGroup(i, col, &after);
            if (after < 0) {
                Diag(Diagnostic::Error, i, 0, Len(s), "unterminated " + Form(sigil, name));
                doc.blocks.push_back(MakeBlock(BlockKind::Paragraph, i, i));
                return i + 1;
            }
            CheckTrailing(i, after);
            Block b = MakeBlock(name == "import" ? BlockKind::Import : BlockKind::Image, i, i);
            b.value = Trim(arg);
            if (b.value.empty()) Diag(Diagnostic::Error, i, 0, Len(s), std::string(1, sigil) + name + " needs a path");
            doc.blocks.push_back(std::move(b));
            return i + 1;
        }
        if (name == "abstract") {
            if (sigil == '\\') return ParseAbstract(i, col);
            Diag(Diagnostic::Error, i, Indent(s), col, "@abstract{...} is now \\abstract(...)");
            return ParseParagraph(i);
        }
        // \citation(key, fields), @citation{key}{fields} or bibtex @article{key, fields}
        const bool paren = sigil == '\\';
        std::string joined;
        std::vector<int> offs;
        int j = i;
        int g1 = -1, g2 = -1;
        for (; j < n && j < i + 500; ++j) {
            offs.push_back(Len(joined));
            joined += L(j);
            joined += '\n';
            g1 = ReadGroup(joined, col, Len(joined));
            if (g1 < 0) continue;
            if (name != "citation" || paren) break;
            int k = g1;
            while (k < Len(joined) && IsSpace(joined[static_cast<size_t>(k)])) ++k;
            if (k >= Len(joined)) continue;
            g2 = ReadGroup(joined, k, Len(joined));
            if (g2 >= 0) break;
        }
        if (j >= n || j >= i + 500 || g1 < 0 || (name == "citation" && !paren && g2 < 0)) {
            Diag(Diagnostic::Error, i, 0, Len(s), "unterminated " + std::string(1, sigil) + name + " entry");
            doc.blocks.push_back(MakeBlock(BlockKind::Paragraph, i, i));
            return i + 1;
        }
        Block b = MakeBlock(BlockKind::Citation, i, j);
        std::string body;
        if (paren) {
            const std::vector<int> comma = ArgCommas(joined, col + 1, g1 - 1, 1);
            const int key_end = comma.empty() ? g1 - 1 : comma[0];
            b.value = Trim(Sub(joined, col + 1, key_end));
            body = comma.empty() ? "" : Sub(joined, key_end + 1, g1 - 1);
            CheckTrailing(j, g1 - offs.back());
        } else if (name == "citation") {
            b.value = Trim(Sub(joined, col + 1, g1 - 1));
            int k = g1;
            while (IsSpace(At(joined, k))) ++k;
            body = Sub(joined, k + 1, g2 - 1);
            CheckTrailing(j, g2 - offs.back());
        } else {
            std::string inner = Sub(joined, col + 1, g1 - 1);
            size_t comma = inner.find(',');
            b.value = Trim(inner.substr(0, comma));
            body = comma == std::string::npos ? "" : inner.substr(comma + 1);
            b.keyword = Lower(name);
            CheckTrailing(j, g1 - offs.back());
        }
        ParseFields(body, &b.fields, &b.field_order);
        if (b.value.empty()) {
            Diag(Diagnostic::Error, i, 0, Len(s), "citation has no key");
        } else {
            if (doc.citations.count(b.value))
                Diag(Diagnostic::Warning, i, 0, Len(s), "duplicate citation key '" + b.value + "'");
            Citation c;
            c.key = b.value;
            c.fields = b.fields;
            c.field_order = b.field_order;
            c.line = i;
            doc.citations[c.key] = c;
        }
        doc.blocks.push_back(std::move(b));
        return j + 1;
    }

    // \abstract( ... ): prose up to the matching parenthesis, over any
    // number of lines; a blank line inside starts a new paragraph.
    int ParseAbstract(int i, int col) {
        const std::string &s = L(i);
        if (At(s, col) != '(') {
            Diag(Diagnostic::Error, i, Indent(s), Len(s), "\\abstract needs a (...) body");
            doc.blocks.push_back(MakeBlock(BlockKind::Paragraph, i, i));
            return i + 1;
        }
        std::string joined;
        int j = i, g = -1;
        for (; j < n; ++j) {
            if (j > i) joined += '\n';
            joined += L(j);
            g = ReadGroup(joined, col, Len(joined));
            if (g >= 0) break;
        }
        if (g < 0) {
            Diag(Diagnostic::Error, i, Indent(s), Len(s), "\\abstract is never closed with )");
            j = n - 1;
        }
        Block b = MakeBlock(BlockKind::Abstract, i, j);
        const int body_end = g < 0 ? Len(b.text) : g - 1;
        if (g >= 0) CheckTrailing(j, g - b.line_offsets.back());
        // Paragraphs: runs of lines with any text, split at blank ones.
        int from = col + 1;
        while (from < body_end) {
            while (from < body_end && IsSpace(b.text[static_cast<size_t>(from)])) ++from;
            if (from >= body_end) break;
            int to = from;
            while (to < body_end) {
                size_t nl = b.text.find('\n', static_cast<size_t>(to));
                if (nl == std::string::npos || static_cast<int>(nl) >= body_end) {
                    to = body_end;
                    break;
                }
                int k = static_cast<int>(nl) + 1;
                int e = k;
                while (e < body_end && b.text[static_cast<size_t>(e)] != '\n' && IsSpace(b.text[static_cast<size_t>(e)])) ++e;
                if (e >= body_end || b.text[static_cast<size_t>(e)] == '\n') {
                    to = static_cast<int>(nl);  // a blank line ends the paragraph
                    break;
                }
                to = k;
            }
            int end = to;
            while (end > from && IsSpace(b.text[static_cast<size_t>(end - 1)])) --end;
            b.paragraph_starts.push_back(b.inlines.size());
            for (Inline &x : Inlines(b, from, end)) b.inlines.push_back(std::move(x));
            from = to;
        }
        if (b.paragraph_starts.empty()) Diag(Diagnostic::Warning, i, Indent(s), Len(s), "empty \\abstract");
        for (const Block &o : doc.blocks)
            if (o.kind == BlockKind::Abstract && o.origin.empty())
                Diag(Diagnostic::Warning, i, Indent(s), col, "a document has one \\abstract; this is a second");
        doc.blocks.push_back(std::move(b));
        return j + 1;
    }

    // A GitHub-flavoured Markdown table starts at `i`: a line with a pipe,
    // over a delimiter row with as many cells. Its rows need no outer pipes.
    // The header is not some other block (a list item, a directive), and
    // the delimiter row is not a list item (`- | -`).
    bool GfmTableAt(int i) {
        if (i + 1 >= n || !GfmRowAt(i) || IsListItem(L(i + 1))) return false;
        size_t delim = 0;
        return IsDelimiterRow(L(i + 1), &delim) && delim == TableCells(L(i)).size();
    }

    // A table starts at `i`: a mepml row (`|` at both ends), or a GFM table.
    bool TableStartAt(int i) { return IsTableRow(Trim(L(i))) || GfmTableAt(i); }

    // Line `j` goes on with a GFM table: any line with a cell pipe that
    // does not start some other block.
    bool GfmRowAt(int j) {
        const std::string &s = L(j);
        const std::string t = Trim(s);
        if (t.empty() || TableCells(s).empty()) return false;
        if (HeadingLevel(s) || IsFence(t) || IsComment(s) || IsListItem(s)) return false;
        if (t.rfind("$$", 0) == 0 || t.rfind("\\[", 0) == 0) return false;
        const std::string d = DirectiveName(s);
        return d.empty() || !IsKnownDirective(d);
    }

    int ParseTable(int i) {
        // mepml's own tables are the lines with a pipe at both ends; a GFM
        // table (header over a delimiter row) also takes rows without them.
        const bool gfm = GfmTableAt(i);
        int j = i + 1;
        while (j < n && (gfm ? GfmRowAt(j) : IsTableRow(Trim(L(j))))) ++j;
        Block b = MakeBlock(BlockKind::Table, i, j - 1);
        b.rows_end = j - 1;
        for (int k = i; k < j; ++k) {
            const std::string &s = L(k);
            int off = b.line_offsets[static_cast<size_t>(k - i)];
            const std::vector<std::pair<int, int>> cells = TableCells(s);
            bool sep = !cells.empty();
            for (auto &cell : cells) {
                std::string t = Trim(Sub(s, cell.first, cell.second));
                bool ok = !t.empty();
                for (char ch : t)
                    if (ch != '-' && ch != ':' && ch != '=') ok = false;
                if (!ok) sep = false;
            }
            if (sep && b.separator_line < 0 && k > i) {
                b.separator_line = k;
                b.header_rows = static_cast<int>(b.rows.size());
                for (auto &cell : cells) {
                    std::string t = Trim(Sub(s, cell.first, cell.second));
                    bool l = t.front() == ':', r = t.back() == ':';
                    b.aligns.push_back(l && r ? Align::Center : r ? Align::Right : l ? Align::Left : Align::Default);
                }
                continue;
            }
            std::vector<TableCell> row;
            for (auto &cell : cells) {
                TableCell tc;
                int cs = cell.first, ce = cell.second;
                while (cs < ce && IsSpace(s[static_cast<size_t>(cs)])) ++cs;
                while (ce > cs && IsSpace(s[static_cast<size_t>(ce - 1)])) --ce;
                tc.start = off + cs;
                tc.end = off + ce;
                tc.content = Inlines(b, tc.start, tc.end);
                ResultImagePath(Sub(s, cs, ce), &tc.image);
                row.push_back(std::move(tc));
            }
            b.rows.push_back(std::move(row));
        }
        doc.blocks.push_back(std::move(b));
        return j;
    }

    int ParseList(int i) {
        int j = i;
        int base_indent = Indent(L(i));
        while (j < n) {
            const std::string &s = L(j);
            if (Trim(s).empty() || IsSlideCloser(s, slide_close)) break;
            if (j > i && !IsListItem(s) && Indent(s) <= base_indent) break;
            ++j;
        }
        Block b = MakeBlock(BlockKind::List, i, j - 1);
        for (int k = i; k < j; ++k) {
            const std::string &s = L(k);
            int off = b.line_offsets[static_cast<size_t>(k - i)];
            ListItem it;
            int mlen = ListMarker(s, &it.ordered, &it.number);
            if (mlen == 0 || IsRule(Trim(s))) {
                // Continuation line of the previous item.
                if (!b.items.empty()) {
                    b.items.back().content_end = off + Len(s);
                }
                continue;
            }
            it.line = k;
            it.indent = Indent(s);
            int cs = mlen;
            if (StartsAt(s, cs, "[ ]") || StartsAt(s, cs, "[x]") || StartsAt(s, cs, "[X]")) {
                it.checkbox = At(s, cs + 1) == ' ' ? 0 : 1;
                cs += 3;
                while (At(s, cs) == ' ') ++cs;
            }
            it.content_start = off + cs;
            it.content_end = off + Len(s);
            b.items.push_back(std::move(it));
        }
        for (ListItem &it : b.items) it.content = Inlines(b, it.content_start, it.content_end);
        doc.blocks.push_back(std::move(b));
        return j;
    }

    bool StartsBlock(int j) {
        const std::string &s = L(j);
        std::string t = Trim(s);
        if (t.empty()) return true;
        if (HeadingLevel(s) || IsFence(t) || IsComment(s) || IsRule(t) || TableStartAt(j) || IsListItem(s)) return true;
        if (t.rfind("$$", 0) == 0 || t.rfind("\\[", 0) == 0) return true;
        if (SlideOpener(s) >= 0 || IsSlideCloser(s, slide_close)) return true;
        std::string d = DirectiveName(s);
        return !d.empty() && IsKnownDirective(d);
    }

    int ParseParagraph(int i) {
        int j = i + 1;
        while (j < n && !StartsBlock(j)) ++j;
        Block b = MakeBlock(BlockKind::Paragraph, i, j - 1);
        b.inlines = Inlines(b, 0, Len(b.text));
        doc.blocks.push_back(std::move(b));
        return j;
    }

    void Run() {
        int i = 0;
        while (i < n) {
            const std::string &s = L(i);
            std::string t = Trim(s);
            if (t.empty() || markdown_result_ends.count(i)) {
                ++i;
                continue;
            }
            char close = 0;
            if (IsSlideCloser(s, slide_close)) {
                Block b = MakeBlock(BlockKind::SlideEnd, i, i);
                b.level = slides;
                doc.blocks.push_back(std::move(b));
                slide_close = 0;
                ++i;
            } else if (const int col = SlideOpener(s, &close); col >= 0) {
                i = ParseSlideOpen(i, col, close);
            } else if (IsMetaLine(s)) {
                i = ParseMetaRun(i);
            } else if (IsComment(s)) {
                i = ParseComment(i);
            } else if (int lvl = HeadingLevel(s)) {
                Block b = MakeBlock(BlockKind::Heading, i, i);
                b.level = lvl;
                int c = lvl;
                while (c < Len(s) && IsSpace(s[static_cast<size_t>(c)])) ++c;
                b.inlines = Inlines(b, c, Len(s));
                doc.blocks.push_back(std::move(b));
                ++i;
            } else if (IsFence(t)) {
                i = ParseCode(i, i);
            } else if (t.rfind("$$", 0) == 0 || t.rfind("\\[", 0) == 0) {
                i = ParseDisplayMath(i);
            } else if (IsRule(t)) {
                doc.blocks.push_back(MakeBlock(BlockKind::Rule, i, i));
                ++i;
            } else if (TableStartAt(i)) {
                i = ParseTable(i);
            } else if (IsListItem(s)) {
                i = ParseList(i);
            } else {
                char sigil = 0;
                std::string d = DirectiveName(s, &sigil);
                if (!d.empty() && IsKnownDirective(d)) {
                    i = ParseDirective(i, d, sigil);
                } else {
                    if (!d.empty())
                        Diag(Diagnostic::Warning, i, Indent(s), Indent(s) + 1 + Len(d),
                             "unknown directive @" + d);
                    i = ParseParagraph(i);
                }
            }
        }
        if (slide_close) SlideNeverClosed();
        PresentationChecks();
        ExportChecks();
    }

    // `//? Export:` names a format the Run button can make; a Beamer deck
    // needs slides to hold.
    void ExportChecks() {
        for (const Block &b : doc.blocks) {
            if (b.kind != BlockKind::Meta || Lower(b.keyword) != "export") continue;
            std::string f = Lower(Trim(b.value));
            if (!f.empty() && f[0] == '.') f.erase(0, 1);
            bool known = false;
            for (const ExportFormat &e : ExportFormats()) known = known || f == e.name;
            if (!known) {
                std::string names;
                for (const ExportFormat &e : ExportFormats()) names += (names.empty() ? "" : " ") + std::string(e.name);
                Diag(Diagnostic::Warning, b.line_start, 0, Len(L(b.line_start)), "unknown export format '" + f + "' (" + names + ")");
            } else if (f == "beamer" && slides == 0) {
                Diag(Diagnostic::Warning, b.line_start, 0, Len(L(b.line_start)), "a Beamer export with no \\slide: the deck is empty");
            }
        }
    }

    // `//? Type:`: an unknown kind is flagged, and in a presentation so is
    // what its slide exports leave out -- content on no slide.
    void PresentationChecks() {
        int type_line = -1;
        std::string type;
        for (const Block &b : doc.blocks)
            if (b.kind == BlockKind::Meta && Lower(b.keyword) == "type") {
                type = Lower(Trim(b.value));
                type_line = b.line_start;
            }
        if (type_line < 0 || type == "document") return;
        if (type != "presentation" && type != "slides") {
            Diag(Diagnostic::Warning, type_line, 0, Len(L(type_line)),
                 "unknown document type '" + type + "' (presentation or document)");
            return;
        }
        if (slides == 0) {
            Diag(Diagnostic::Warning, type_line, 0, Len(L(type_line)), "a presentation with no \\slide: its slide exports are empty");
            return;
        }
        bool on_slide = false;
        for (const Block &b : doc.blocks) {
            if (b.kind == BlockKind::SlideBegin) on_slide = true;
            else if (b.kind == BlockKind::SlideEnd) on_slide = false;
            else if (!on_slide && b.kind != BlockKind::Meta && b.kind != BlockKind::Comment && b.kind != BlockKind::Import &&
                     b.kind != BlockKind::Citation)
                Diag(Diagnostic::Info, b.line_start, 0, Len(L(b.line_start)),
                     "not on any slide: a presentation's slide exports leave it out");
        }
    }

    // The slide open now (its closing bracket, 0 when none is), the line
    // that opened it, and how many slides have opened so far.
    char slide_close = 0;
    int slide_line = -1;
    int slides = 0;

    void SlideNeverClosed() {
        const std::string &s = L(slide_line);
        Diag(Diagnostic::Error, slide_line, Indent(s), Len(s),
             std::string("\\slide is never closed with a line holding just ") + slide_close);
    }

    // \slide( on a line of its own: the slide's content is the blocks that
    // follow, up to a line holding just its `)`. Slides do not nest: one
    // opening while another is open ends that one there (reported).
    int ParseSlideOpen(int i, int col, char close) {
        if (slide_close) SlideNeverClosed();
        Block b = MakeBlock(BlockKind::SlideBegin, i, i);
        b.level = ++slides;
        const std::string rest = Trim(Sub(L(i), col, Len(L(i))));
        if (!rest.empty() && rest.rfind("//", 0) != 0)
            Diag(Diagnostic::Warning, i, col, Len(L(i)),
                 std::string("a slide's content goes on the lines after its opener, up to a line holding just ") + close);
        doc.blocks.push_back(std::move(b));
        slide_close = close;
        slide_line = i;
        return i + 1;
    }
};

void CollectCites(const std::vector<Inline> &ins, const Block &b, Document &doc, std::set<std::string> &seen) {
    for (const Inline &x : ins) {
        if (x.kind == InlineKind::Cite || x.kind == InlineKind::CiteP) {
            if (!doc.citations.count(x.text)) {
                if (b.origin.empty()) {
                    Block::Pos p = b.OffsetToPos(x.start);
                    Block::Pos q = b.OffsetToPos(x.end);
                    Diagnostic d;
                    d.severity = Diagnostic::Warning;
                    d.line = p.line;
                    d.col_start = p.col;
                    d.col_end = q.line == p.line ? q.col : p.col + 1;
                    d.message = "unknown citation key '" + x.text + "'";
                    doc.diagnostics.push_back(d);
                }
            } else if (!seen.count(x.text)) {
                seen.insert(x.text);
                doc.cite_order.push_back(x.text);
            }
        }
        CollectCites(x.children, b, doc, seen);
    }
}

void Finish(Document &doc) {
    std::set<std::string> seen;
    doc.cite_order.clear();
    doc.footnote_count = 0;
    // Diagnostics about unknown keys are regenerated here.
    doc.diagnostics.erase(std::remove_if(doc.diagnostics.begin(), doc.diagnostics.end(),
                                         [](const Diagnostic &d) {
                                             return d.message.rfind("unknown citation key", 0) == 0;
                                         }),
                          doc.diagnostics.end());
    int fn = 0;
    std::function<void(std::vector<Inline> &)> renumber = [&](std::vector<Inline> &ins) {
        for (Inline &x : ins) {
            if (x.kind == InlineKind::Footnote) x.number = ++fn;
            renumber(x.children);
        }
    };
    for (Block &b : doc.blocks) {
        renumber(b.inlines);
        renumber(b.caption_inlines);
        for (ListItem &it : b.items) renumber(it.content);
        for (auto &row : b.rows)
            for (TableCell &c : row) renumber(c.content);
        CollectCites(b.inlines, b, doc, seen);
        CollectCites(b.caption_inlines, b, doc, seen);
        for (const ListItem &it : b.items) CollectCites(it.content, b, doc, seen);
        for (const auto &row : b.rows)
            for (const TableCell &c : row) CollectCites(c.content, b, doc, seen);
    }
    doc.footnote_count = fn;
}

}  // namespace

int LineHeadingLevel(const std::string &line) { return HeadingLevel(line); }

std::string MetaValue(const Document &doc, const std::string &key) {
    std::string v;
    const std::string k = Lower(key);
    for (const auto &kv : doc.meta)
        if (Lower(kv.first) == k) v = Trim(kv.second);
    return v;
}

bool IsPresentation(const Document &doc) {
    const std::string t = Lower(MetaValue(doc, "type"));
    return t == "presentation" || t == "slides";
}

const std::vector<ExportFormat> &ExportFormats() {
    static const std::vector<ExportFormat> v = {
        {"html", "HTML (the default); a presentation's is a slideshow"},
        {"pdf", "PDF, LaTeX compiled by tectonic; a presentation's is a Beamer deck"},
        {"beamer", "a Beamer slide deck as PDF, whatever the document's Type"},
        {"docx", "Word"},
        {"odt", "OpenDocument text"},
        {"rtf", "Rich Text"},
        {"md", "Markdown"},
        {"org", "Org"},
        {"tex", "LaTeX source; a presentation's is a Beamer deck"},
        {"txt", "plain text"},
        {"pptx", "PowerPoint slides"},
        {"odp", "Impress slides"},
        {"markdown", "Markdown (md)"},
        {"latex", "LaTeX source (tex)"},
        {"text", "plain text (txt)"},
        {"powerpoint", "PowerPoint slides (pptx)"},
        {"impress", "Impress slides (odp)"},
    };
    return v;
}

std::vector<Slide> Slides(const Document &doc, int line_count) {
    std::vector<Slide> out;
    bool open = false;
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block &b = doc.blocks[i];
        if (!b.origin.empty()) continue;
        if (b.kind == BlockKind::SlideBegin) {
            if (open) {
                out.back().line_end = b.line_start - 1;
                out.back().last_block = i;
            }
            Slide sl;
            sl.number = b.level;
            sl.line_start = b.line_start;
            sl.first_block = i;
            out.push_back(sl);
            open = true;
        } else if (open && b.kind == BlockKind::SlideEnd) {
            out.back().line_end = b.line_start;
            out.back().closed = true;
            out.back().last_block = i + 1;
            open = false;
        } else if (open && b.kind == BlockKind::Heading && out.back().title.empty()) {
            out.back().title = InlinePlainText(b.inlines);
        }
    }
    if (open) {
        out.back().line_end = std::max(out.back().line_start, line_count - 1);
        out.back().last_block = doc.blocks.size();
    }
    return out;
}

std::vector<std::vector<Inline>> AbstractParagraphs(const Block &b) {
    std::vector<std::vector<Inline>> out;
    for (size_t k = 0; k < b.paragraph_starts.size(); ++k) {
        const size_t from = b.paragraph_starts[k];
        const size_t to = k + 1 < b.paragraph_starts.size() ? b.paragraph_starts[k + 1] : b.inlines.size();
        out.emplace_back(b.inlines.begin() + static_cast<std::ptrdiff_t>(from),
                         b.inlines.begin() + static_cast<std::ptrdiff_t>(std::min(to, b.inlines.size())));
    }
    return out;
}

bool ResultImagePath(const std::string &text, std::string *path) {
    std::string t = Trim(text);
    if (!((t.rfind("\\image(", 0) == 0 && t.back() == ')') || (t.rfind("@image{", 0) == 0 && t.back() == '}')))
        return false;
    *path = Trim(t.substr(7, t.size() - 8));
    return !path->empty();
}

std::vector<std::string> BlockLabels(const Document &doc) {
    std::vector<std::string> out(doc.blocks.size());
    int figures = 0, tables = 0;
    // What the exports leave out is not numbered, in the editor either, so
    // "Table 3" in the prose means the same table everywhere.
    const std::vector<bool> hidden = ExportHidden(doc);
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block &b = doc.blocks[i];
        if (hidden[i]) continue;
        bool code = true, results = true;
        if (b.kind == BlockKind::Code) CodeExports(doc, b, &code, &results);
        if (b.kind == BlockKind::Image || (b.kind == BlockKind::Code && !b.result_images.empty() && results))
            out[i] = "Figure " + std::to_string(++figures);
        else if (b.kind == BlockKind::Table)
            out[i] = "Table " + std::to_string(++tables);
    }
    return out;
}

int LineHeadingMarkupLen(const std::string &line) {
    int lvl = HeadingLevel(line);
    if (lvl == 0) return 0;
    int c = lvl;
    while (c < Len(line) && (line[static_cast<size_t>(c)] == ' ' || line[static_cast<size_t>(c)] == '\t')) ++c;
    return c;
}

Document Parse(const std::vector<std::string> &lines) {
    Parser p(lines);
    p.Run();
    Finish(p.doc);
    return std::move(p.doc);
}

// ---------------------------------------------------------------------------
// Imports

std::string ResolvePath(const std::string &base_file, const std::string &path) {
    if (path.empty() || path[0] == '/') return path;
    std::string p = path;
    if (p.rfind("~/", 0) == 0) {
        const char *home = std::getenv("HOME");
        return std::string(home ? home : "") + p.substr(1);
    }
    size_t slash = base_file.rfind('/');
    std::string dir = slash == std::string::npos ? "" : base_file.substr(0, slash + 1);
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    return dir + p;
}

namespace {

// `included` holds every file already expanded into this document: a
// file imported twice (by the header and again in the body, say) is
// included once, where it is first named.
void Expand(const std::string &file, Document &doc, const ReadFileFn &read, std::vector<std::string> &stack,
            std::set<std::string> &included, bool top) {
    std::vector<Block> out;
    for (Block &b : doc.blocks) {
        // `\import(path)` in the body, or `//? Import: path` in the header:
        // either way the file's blocks follow the line that names it, so
        // header imports land in the order the header lists them.
        const bool header = b.kind == BlockKind::Meta && Lower(b.keyword) == "import";
        if (b.kind != BlockKind::Import && !header) {
            out.push_back(std::move(b));
            continue;
        }
        const std::string target = Trim(b.value);
        if (target.empty()) {  // reported by the parser
            out.push_back(std::move(b));
            continue;
        }
        std::string path = ResolvePath(file, target);
        // An imported file's own import failures are recorded too, and
        // surface on the line of this document that led to them.
        auto diag = [&](const std::string &msg) {
            Diagnostic d;
            d.severity = Diagnostic::Error;
            d.line = b.line_start;
            d.col_start = 0;
            d.col_end = static_cast<int>(b.text.size());
            d.message = msg;
            doc.diagnostics.push_back(d);
        };
        if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
            diag("circular import of " + target);
            out.push_back(std::move(b));
            continue;
        }
        if (included.count(path)) {
            if (top) {
                Diagnostic d;
                d.severity = Diagnostic::Info;
                d.line = b.line_start;
                d.col_start = 0;
                d.col_end = static_cast<int>(b.text.size());
                d.message = "already imported " + target + " above; it is included only once";
                doc.diagnostics.push_back(d);
            }
            out.push_back(std::move(b));
            continue;
        }
        included.insert(path);
        std::vector<std::string> sub_lines;
        if (!read(path, &sub_lines)) {
            diag("cannot read import " + target);
            out.push_back(std::move(b));
            continue;
        }
        Parser p(sub_lines);
        p.Run();
        stack.push_back(path);
        Expand(path, p.doc, read, stack, included, false);
        stack.pop_back();
        for (const Diagnostic &sd : p.doc.diagnostics) {
            if (sd.message.rfind("circular import", 0) != 0 && sd.message.rfind("cannot read import", 0) != 0 &&
                sd.message.rfind("in ", 0) != 0)
                continue;
            Diagnostic d = sd;
            d.line = b.line_start;
            d.col_start = 0;
            d.col_end = static_cast<int>(b.text.size());
            d.message = "in " + target + ": " + sd.message;
            doc.diagnostics.push_back(d);
        }
        out.push_back(std::move(b));  // keep the directive itself (renders as nothing)
        for (Block &sb : p.doc.blocks) {
            if (sb.origin.empty()) sb.origin = path;
            out.push_back(std::move(sb));
        }
        for (auto &kv : p.doc.citations)
            if (!doc.citations.count(kv.first)) doc.citations[kv.first] = kv.second;
        // Its header is inherited, and the importing document's own wins:
        // an option this document sets stays as it is, the title only
        // arrives if this document has none, and any other key (Author,
        // Date, ...) only if this document does not have it. `Import`
        // lines are not inherited -- they have already been expanded.
        std::set<std::string> own_keys;
        for (const auto &kv : doc.meta) own_keys.insert(Lower(kv.first));
        for (const auto &kv : p.doc.meta) {
            const std::string k = Lower(kv.first);
            if (k == "import") continue;
            if (k == "option") {
                Option o;
                if (!ParseAssignment(kv.second, &o) || doc.FindOption(o.name)) continue;
                doc.meta.push_back(kv);
                continue;
            }
            if (k == "title") {
                if (!doc.title.empty()) continue;
                doc.title = kv.second;
            } else if (own_keys.count(k)) {
                continue;
            }
            doc.meta.push_back(kv);
        }
        for (const Option &o : p.doc.options)
            if (!doc.FindOption(o.name)) doc.options.push_back(o);
    }
    doc.blocks = std::move(out);
}

}  // namespace

Document ParseWithImports(const std::string &file, const std::vector<std::string> &lines, const ReadFileFn &read) {
    Parser p(lines);
    p.Run();
    std::vector<std::string> stack{file};
    std::set<std::string> included;
    Expand(file, p.doc, read, stack, included, true);
    Finish(p.doc);
    return std::move(p.doc);
}

// ---------------------------------------------------------------------------
// Results

std::string HtmlResultFragment(const std::string &html) {
    const std::string lower = Lower(html);
    const size_t body = lower.find("<body");
    if (body == std::string::npos) {
        // A fragment -- but drop a stray doctype.
        const size_t dt = lower.find("<!doctype");
        if (dt == std::string::npos) return html;
        const size_t gt = html.find('>', dt);
        return gt == std::string::npos ? html : html.substr(0, dt) + html.substr(gt + 1);
    }
    std::string out;
    for (size_t at = lower.find("<style"); at != std::string::npos && at < body; at = lower.find("<style", at + 1)) {
        const size_t end = lower.find("</style>", at);
        if (end == std::string::npos) break;
        out += html.substr(at, end + 8 - at) + "\n";
    }
    const size_t open_end = html.find('>', body);
    if (open_end == std::string::npos) return html;
    const size_t close = lower.rfind("</body>");
    out += html.substr(open_end + 1, close == std::string::npos || close < open_end ? std::string::npos : close - open_end - 1);
    return out;
}

std::string ResultFormatFor(const Block &b) {
    for (const Option &o : b.options) {
        const std::string n = Lower(o.name);
        if (n != "results" && n != "output") continue;
        const std::string v = Lower(Trim(o.value.s));
        if (v.find("html") != std::string::npos) return "html";
        // knitr's `results='asis'` and org's `:results raw` mean the same.
        for (const char *w : {"markdown", "md", "asis", "raw"}) {
            const size_t at = v.find(w);
            const size_t end = at + std::string(w).size();
            if (at != std::string::npos && (at == 0 || !IsAlpha(v[at - 1])) && (end >= v.size() || !IsAlpha(v[end])))
                return "markdown";
        }
    }
    return "";
}

std::vector<std::string> FormatResults(const std::string &output, const std::string &format) {
    std::vector<std::string> out;
    out.push_back(format.empty() ? "// result_begin:" : "// result_begin: " + format);
    std::string o = output;
    while (!o.empty() && (o.back() == '\n' || o.back() == '\r')) o.pop_back();
    const bool raw = format == "markdown";
    // Markdown is written as it is, to be read as the document's own; the
    // blank lines a printer puts before and after a table go.
    if (raw) {
        size_t lead = 0;
        while (lead < o.size() && (o[lead] == '\n' || o[lead] == '\r')) ++lead;
        o.erase(0, lead);
    }
    size_t k = 0;
    if (!o.empty()) {
        while (true) {
            size_t e = o.find('\n', k);
            std::string ln = o.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (raw) out.push_back(ln);
            else out.push_back(ln.empty() ? "//" : "// " + ln);
            if (e == std::string::npos) break;
            k = e + 1;
        }
    }
    out.push_back("// result_end");
    return out;
}

void ResultsReplaceRange(const Block &b, int *first, int *last) {
    if (b.result_line_start >= 0) {
        *first = b.result_line_start;
        *last = b.result_line_end + 1;
    } else {
        int close = b.code_line_end + 1;
        *first = *last = close + 1;
    }
}

// ---------------------------------------------------------------------------
// What the exports show of a code block

void CodeExports(const Document &doc, const Block &b, bool *code, bool *results) {
    // org-babel's :exports, from the document's header (`//? Exports:` or an
    // `exports` option), then the block's own `exports=` or knitr's `echo=`.
    std::string mode;
    for (const auto &kv : doc.meta)
        if (Lower(Trim(kv.first)) == "exports") mode = Lower(Trim(kv.second));
    if (const Option *o = doc.FindOption("exports")) mode = Lower(Trim(o->value.s));
    for (const Option &o : b.options) {
        const std::string n = Lower(o.name), v = Lower(Trim(o.value.s));
        if (n == "exports") mode = v;
        if (n == "echo") mode = v == "false" || v == "no" || v == "0" || v == "nil" ? "results" : "both";
    }
    *code = mode != "results" && mode != "none";
    *results = mode != "code" && mode != "none";
}

std::vector<bool> ExportHidden(const Document &doc) {
    std::vector<bool> hide(doc.blocks.size(), false);
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block &c = doc.blocks[i];
        if (c.kind != BlockKind::Code || c.result_format != "markdown" || c.result_line_start < 0) continue;
        bool code = true, results = true;
        CodeExports(doc, c, &code, &results);
        if (results) continue;
        for (size_t j = i + 1; j < doc.blocks.size() && doc.blocks[j].origin == c.origin &&
                               doc.blocks[j].line_start < c.result_line_end;
             ++j)
            hide[j] = true;
    }
    return hide;
}

// ---------------------------------------------------------------------------
// Tables

std::vector<std::pair<int, int>> TableCells(const std::string &s) {
    std::vector<int> pipes;
    bool tick = false;
    for (int c = 0; c < Len(s); ++c) {
        char ch = s[static_cast<size_t>(c)];
        if (ch == '\\') {
            ++c;
            continue;
        }
        if (ch == '`') tick = !tick;
        if (ch == '|' && !tick) pipes.push_back(c);
    }
    std::vector<std::pair<int, int>> cells;
    if (pipes.empty()) return cells;
    int first = Indent(s);
    int last = Len(s);
    while (last > first && IsSpace(s[static_cast<size_t>(last - 1)])) --last;
    const bool lead = pipes.front() == first, trail = pipes.back() == last - 1;
    int cb = lead ? pipes.front() + 1 : first;
    for (size_t k = lead ? 1 : 0; k < pipes.size(); ++k) {
        cells.emplace_back(cb, pipes[k]);
        cb = pipes[k] + 1;
    }
    if (!trail) cells.emplace_back(cb, last);
    return cells;
}

}  // namespace mepml

// ---------------------------------------------------------------------------
// Highlight

namespace mepml {

namespace {

std::uint32_t FlagFor(InlineKind k) {
    switch (k) {
        case InlineKind::Bold: return kBold;
        case InlineKind::Italic: return kItalic;
        case InlineKind::Underline: return kUnderline;
        case InlineKind::Superscript: return kSuper;
        case InlineKind::Subscript: return kSub;
        case InlineKind::Small: return kSmall;
        case InlineKind::Big: return kBig;
        case InlineKind::Mono: return kMono;
        case InlineKind::Highlight: return kHighlight;
        case InlineKind::Strike: return kStrike;
        case InlineKind::Insert: return kInsert;
        case InlineKind::Delete: return kDelete;
        case InlineKind::Verbatim: return kVerbatim;
        case InlineKind::Link: return kLink;
        case InlineKind::Math: return kMath;
        case InlineKind::Comment: return kComment;
        case InlineKind::Footnote: return kFootnote;
        case InlineKind::Cite:
        case InlineKind::CiteP: return kCite;
        default: return 0;
    }
}

const char *const kSuperDigits[] = {"⁰", "¹", "²", "³", "⁴",
                                    "⁵", "⁶", "⁷", "⁸", "⁹"};
std::string SuperNumber(int n) {
    std::string d = std::to_string(n), out;
    for (char c : d) out += kSuperDigits[c - '0'];
    return out;
}

struct Emitter {
    const Document &doc;
    const Block *b = nullptr;
    std::vector<Span> out;

    // One span per line crossed by [from, to) of the current block's text.
    void Range(int from, int to, const Span &proto) {
        if (to <= from) return;
        Block::Pos p = b->OffsetToPos(from);
        Block::Pos q = b->OffsetToPos(to);
        for (int line = p.line; line <= q.line; ++line) {
            Span s = proto;
            s.line = line;
            s.col_start = line == p.line ? p.col : 0;
            if (line == q.line) {
                s.col_end = q.col;
            } else {
                int idx = line - b->line_start;
                int line_len = (static_cast<size_t>(idx + 1) < b->line_offsets.size()
                                    ? b->line_offsets[static_cast<size_t>(idx + 1)] - 1
                                    : static_cast<int>(b->text.size())) -
                               b->line_offsets[static_cast<size_t>(idx)];
                s.col_end = line_len;
            }
            if (s.col_end > s.col_start) out.push_back(std::move(s));
            // The replacement text belongs to the first piece only.
            if (!proto.replace.empty()) break;
        }
    }
    void Line(int line, int cb, int ce, const Span &proto) {
        if (ce <= cb) return;
        Span s = proto;
        s.line = line;
        s.col_start = cb;
        s.col_end = ce;
        out.push_back(std::move(s));
    }

    void Inlines(const std::vector<Inline> &ins, const Span &base) {
        for (const Inline &x : ins) Node(x, base);
    }

    void Node(const Inline &x, const Span &base) {
        Span st = base;
        st.style |= FlagFor(x.kind);
        st.markup = false;
        st.replace.clear();
        if (x.kind == InlineKind::Color) st.color = x.arg;
        if (x.kind == InlineKind::Font) st.font = x.arg;
        if (x.kind == InlineKind::FontSize) st.font_size = static_cast<float>(std::atof(x.arg.c_str()));
        if (x.kind == InlineKind::Link) st.target = x.arg;
        Span mk = st;
        mk.markup = true;

        switch (x.kind) {
            case InlineKind::Text:
                if (x.inner_start > x.start) Range(x.start, x.inner_start, mk);  // escape backslash
                Range(x.inner_start, x.inner_end, st);
                return;
            case InlineKind::Cite:
            case InlineKind::CiteP: {
                bool known = doc.citations.count(x.text) > 0;
                mk.target = x.text;
                if (!known) mk.style |= kError;
                mk.replace = known ? CiteLabel(doc, x.text, x.kind == InlineKind::CiteP)
                                   : "[?" + x.text + "]";
                Range(x.start, x.end, mk);
                return;
            }
            case InlineKind::Footnote: {
                Span open = mk;
                open.replace = SuperNumber(x.number) + "(";
                Range(x.start, x.inner_start, open);
                Inlines(x.children, st);
                Span close = mk;
                close.replace = ")";
                Range(x.inner_end, x.end, close);
                return;
            }
            case InlineKind::Math:
            case InlineKind::Verbatim:
            case InlineKind::Comment: {
                Range(x.start, x.inner_start, mk);
                Range(x.inner_start, x.inner_end, st);
                Range(x.inner_end, x.end, mk);
                return;
            }
            default:
                Range(x.start, x.inner_start, mk);
                Inlines(x.children, st);
                Range(x.inner_end, x.end, mk);
                return;
        }
    }

    // Whole lines of a block in one style, split at the `//` prefix when
    // `prefix_markup`.
    void Lines(int from, int to, const Span &proto) {
        for (int line = from; line <= to; ++line) {
            int idx = line - b->line_start;
            int len = (static_cast<size_t>(idx + 1) < b->line_offsets.size()
                           ? b->line_offsets[static_cast<size_t>(idx + 1)] - 1
                           : static_cast<int>(b->text.size())) -
                      b->line_offsets[static_cast<size_t>(idx)];
            Line(line, 0, len, proto);
        }
    }
    const std::string LineText(int line) const {
        int idx = line - b->line_start;
        int from = b->line_offsets[static_cast<size_t>(idx)];
        int to = static_cast<size_t>(idx + 1) < b->line_offsets.size() ? b->line_offsets[static_cast<size_t>(idx + 1)] - 1
                                                                        : static_cast<int>(b->text.size());
        return b->text.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
    }

    // Where a directive line's `\` (or `@`) is, and its group's `(` (or
    // `{`) after it; -1 for either that is missing.
    static int SigilAt(const std::string &s) {
        const int i = Indent(s);
        return At(s, i) == '@' || At(s, i) == '\\' ? i : -1;
    }
    static int OpenerAt(const std::string &s, int at) {
        return static_cast<int>(s.find_first_of("{(", static_cast<size_t>(at < 0 ? 0 : at)));
    }

    // `\name(arg)` directive line: name+parentheses markup, arg styled.
    void Directive(int line, const std::string &target) {
        std::string s = LineText(line);
        int at = SigilAt(s);
        int brace = OpenerAt(s, at);
        Span d;
        d.style = kDirective;
        d.target = target;
        if (at < 0) return;
        if (brace < 0) {
            Line(line, at, static_cast<int>(s.size()), d);
            return;
        }
        int g = ReadGroup(s, brace, static_cast<int>(s.size()));
        if (g < 0) g = static_cast<int>(s.size());
        Line(line, at, g, d);
        TrailingComment(line, g);
    }
    // A \caption(...) / \alttext(...) under a block, over one line or
    // several: the `@name{` opener and closing `}` are markup (a caption's
    // opener reads "Caption: " concealed), a caption's text is styled
    // inline, an alt text's is the directive colour throughout.
    void Attribute(const Block &blk, int first, int last, int close_col, bool caption) {
        const std::string s0 = LineText(first);
        const int at = SigilAt(s0);
        const int brace = OpenerAt(s0, at);
        if (at < 0 || brace < 0 || last < first || close_col < 0) return;
        Span d;
        d.style = kDirective;
        d.markup = true;
        d.replace = caption ? "Caption: " : "";
        Line(first, at, brace + 1, d);
        if (caption) {
            Span cap;
            cap.style = kItalic;
            Inlines(blk.caption_inlines, cap);
        } else {
            Span a;
            a.style = kDirective;
            for (int line = first; line <= last; ++line) {
                const int from = line == first ? brace + 1 : 0;
                const int to = line == last ? close_col : static_cast<int>(LineText(line).size());
                Line(line, from, to, a);
            }
        }
        Span close = d;
        close.replace.clear();
        Line(last, close_col, close_col + 1, close);
        TrailingComment(last, close_col + 1);
    }
    void TrailingComment(int line, int col) {
        std::string s = LineText(line);
        size_t c = s.find("//", static_cast<size_t>(col));
        if (c == std::string::npos) return;
        Span cm;
        cm.style = kComment;
        std::string kw = CalloutKeyword(s.substr(c));
        if (!kw.empty()) {
            cm.style |= kCallout;
            cm.callout = kw;
        }
        Line(line, static_cast<int>(c), static_cast<int>(s.size()), cm);
    }

    void Block_(const Block &blk) {
        b = &blk;
        Span none;
        switch (blk.kind) {
            case BlockKind::Paragraph:
                Inlines(blk.inlines, none);
                break;
            case BlockKind::Heading: {
                Span h;
                h.style = kHeading;
                h.heading_level = blk.level;
                Span mk = h;
                mk.markup = true;
                int c = blk.level;
                while (c < static_cast<int>(blk.text.size()) && (blk.text[static_cast<size_t>(c)] == ' ')) ++c;
                Line(blk.line_start, 0, c, mk);
                Inlines(blk.inlines, h);
                break;
            }
            case BlockKind::Comment: {
                Span c;
                c.style = kComment;
                Lines(blk.line_start, blk.line_end, c);
                break;
            }
            case BlockKind::Callout: {
                Span c;
                c.style = kCallout;
                c.callout = blk.keyword;
                // "// NOTE:" badge on the first line, "//" on the rest.
                for (int line = blk.line_start; line <= blk.line_end; ++line) {
                    std::string s = LineText(line);
                    int p = static_cast<int>(s.find("//"));
                    int end = p + 2;
                    if (line == blk.line_start) end = static_cast<int>(s.find(':', static_cast<size_t>(end))) + 1;
                    while (end < static_cast<int>(s.size()) && s[static_cast<size_t>(end)] == ' ') ++end;
                    Span mk = c;
                    mk.markup = true;
                    mk.replace = line == blk.line_start ? " " + blk.keyword + " " : "";
                    if (line != blk.line_start) mk.replace = std::string(1, ' ');
                    Line(line, p, end, mk);
                }
                Inlines(blk.inlines, c);
                break;
            }
            case BlockKind::Meta: {
                std::string s = LineText(blk.line_start);
                Span m;
                m.style = kMeta;
                Line(blk.line_start, 0, static_cast<int>(s.size()), m);
                break;
            }
            case BlockKind::Import:
                Directive(blk.line_start, blk.value);
                break;
            case BlockKind::Image:
            case BlockKind::Table:
            case BlockKind::MathBlock:
            case BlockKind::Code: {
                int body_end = blk.line_end;
                if (blk.caption_line >= 0) body_end = std::min(body_end, blk.caption_line - 1);
                if (blk.alt_line >= 0) body_end = std::min(body_end, blk.alt_line - 1);
                if (blk.kind == BlockKind::Table && blk.rows_end >= 0) body_end = std::min(body_end, blk.rows_end);
                if (blk.kind == BlockKind::Image) {
                    Directive(blk.line_start, blk.value);
                } else if (blk.kind == BlockKind::MathBlock) {
                    Span m;
                    m.style = kMath;
                    Lines(blk.line_start, body_end, m);
                } else if (blk.kind == BlockKind::Table) {
                    TableSpans(blk, body_end);
                } else {
                    CodeSpans(blk);
                }
                if (blk.caption_line >= 0) Attribute(blk, blk.caption_line, blk.caption_line_end, blk.caption_close_col, true);
                if (blk.alt_line >= 0) Attribute(blk, blk.alt_line, blk.alt_line_end, blk.alt_close_col, false);
                break;
            }
            case BlockKind::Citation: {
                Span d;
                d.style = kDirective;
                d.target = blk.value;
                Lines(blk.line_start, blk.line_end, d);
                break;
            }
            case BlockKind::List: {
                for (const ListItem &it : blk.items) {
                    std::string s = LineText(it.line);
                    int off = blk.line_offsets[static_cast<size_t>(it.line - blk.line_start)];
                    Span mk;
                    mk.style = kListMarker;
                    mk.markup = true;
                    int mend = it.content_start - off;
                    if (it.checkbox >= 0) mk.replace = it.checkbox ? "✓ " : "□ ";
                    else if (!it.ordered) mk.replace = "• ";
                    if (!mk.replace.empty()) {
                        mk.replace = std::string(static_cast<size_t>(it.indent), ' ') + mk.replace;
                        Line(it.line, 0, mend, mk);
                    } else {
                        Span om = mk;
                        om.markup = false;
                        Line(it.line, it.indent, mend, om);
                    }
                    Span content;
                    if (it.checkbox == 1) content.style |= kStrike;
                    Inlines(it.content, content);
                }
                break;
            }
            case BlockKind::Rule: {
                Span r;
                r.style = kRule;
                r.markup = true;
                Lines(blk.line_start, blk.line_end, r);
                break;
            }
            case BlockKind::Bibliography:
            case BlockKind::TableOfContents:
                Directive(blk.line_start, "");
                break;
            case BlockKind::Abstract: {
                // `\abstract(` reads as the section's label -- on a line of
                // its own, or run in before the text that follows it -- and
                // the closing `)` goes away.
                Span mk;
                mk.style = kDirective | kAbstract;
                mk.markup = true;
                std::string s = LineText(blk.line_start);
                int at = SigilAt(s);
                int open = OpenerAt(s, at);
                if (at < 0 || open < 0) break;
                bool alone = Trim(s.substr(static_cast<size_t>(open) + 1)).empty();
                mk.replace = alone ? "Abstract" : "Abstract. ";
                Line(blk.line_start, at, open + 1, mk);
                Inlines(blk.inlines, none);
                int g = ReadGroup(blk.text, blk.line_offsets[0] + open, static_cast<int>(blk.text.size()));
                if (g > 0) {
                    Span close = mk;
                    close.replace.clear();
                    Range(g - 1, g, close);
                    Block::Pos p = blk.OffsetToPos(g);
                    TrailingComment(p.line, p.col);
                }
                break;
            }
            case BlockKind::SlideBegin:
            case BlockKind::SlideEnd: {
                // The opener reads as the slide's "Slide N" label, the
                // closing bracket goes away (the editor draws both as rules).
                const std::string s = LineText(blk.line_start);
                const int at = Indent(s);
                const int to = blk.kind == BlockKind::SlideBegin ? SlideOpener(s) : at + 1;
                if (to < 0) break;
                Span mk;
                mk.style = kDirective | kSlide;
                mk.markup = true;
                if (blk.kind == BlockKind::SlideBegin) mk.replace = "Slide " + std::to_string(blk.level);
                Line(blk.line_start, at, to, mk);
                TrailingComment(blk.line_start, to);
                break;
            }
        }
    }

    void TableSpans(const Block &blk, int body_end) {
        for (int line = blk.line_start; line <= body_end; ++line) {
            std::string s = LineText(line);
            Span t;
            t.style = kTable | kTableRule;
            t.markup = true;
            if (line == blk.separator_line) {
                Line(line, 0, static_cast<int>(s.size()), t);
                continue;
            }
            for (size_t c = 0; c < s.size(); ++c) {
                if (s[c] == '\\') {
                    ++c;
                    continue;
                }
                if (s[c] == '|') Line(line, static_cast<int>(c), static_cast<int>(c) + 1, t);
            }
        }
        size_t row_idx = 0;
        for (const auto &row : blk.rows) {
            Span cell;
            cell.style = kTable;
            if (static_cast<int>(row_idx) < blk.header_rows) cell.style |= kBold;
            for (const TableCell &c : row) {
                if (c.image.empty()) {
                    Inlines(c.content, cell);
                    continue;
                }
                // A picture cell reads as one directive (the editor draws
                // the image over it), not as inline markup in a path.
                Span d;
                d.style = kTable | kDirective;
                d.target = c.image;
                Range(c.start, c.end, d);
            }
            ++row_idx;
        }
    }

    void CodeSpans(const Block &blk) {
        Span m;
        m.style = kMeta;
        for (int line = blk.line_start; line < blk.code_line_start - 1; ++line) Lines(line, line, m);
        Span fence;
        fence.style = kCode | kDirective;
        Lines(blk.code_line_start - 1, blk.code_line_start - 1, fence);
        Span body;
        body.style = kCode;
        if (blk.code_line_end >= blk.code_line_start) Lines(blk.code_line_start, blk.code_line_end, body);
        int close = blk.code_line_end + 1;
        if (close <= blk.line_end && close != blk.result_line_start) Lines(close, close, fence);
        if (blk.result_line_start >= 0) {
            Span r;
            r.style = kResult;
            Span rm = r;
            rm.style |= kComment;
            Lines(blk.result_line_start, blk.result_line_start, rm);
            // Markdown results are the document's own blocks, styled as such.
            for (int line = blk.result_line_start + 1; line < blk.result_line_end && blk.result_format != "markdown"; ++line) {
                std::string s = LineText(line);
                size_t p = s.find("//");
                if (p == std::string::npos) {
                    Lines(line, line, r);
                    continue;
                }
                int end = static_cast<int>(p) + 2;
                if (end < static_cast<int>(s.size()) && s[static_cast<size_t>(end)] == ' ') ++end;
                Span pm = r;
                pm.markup = true;
                Line(line, 0, end, pm);
                // A figure the block produced: the editor draws the image
                // on this row (Span::target is its path).
                Span content = r;
                std::string img;
                if (ResultImagePath(s.substr(static_cast<size_t>(end)), &img)) {
                    content.style |= kDirective;
                    content.target = img;
                }
                Line(line, end, static_cast<int>(s.size()), content);
            }
            if (blk.result_line_end > blk.result_line_start) Lines(blk.result_line_end, blk.result_line_end, rm);
        }
    }
};

}  // namespace

std::vector<Span> Highlight(const Document &doc) {
    Emitter em{doc, nullptr, {}};
    for (const Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        em.Block_(b);
    }
    std::stable_sort(em.out.begin(), em.out.end(), [](const Span &a, const Span &c) {
        return a.line != c.line ? a.line < c.line : a.col_start < c.col_start;
    });
    return std::move(em.out);
}

std::string CiteLabel(const Document &doc, const std::string &key, bool parenthetical) {
    auto it = doc.citations.find(key);
    if (it == doc.citations.end()) return "[?" + key + "]";
    const auto &f = it->second.fields;
    std::string author = f.count("author") ? f.at("author") : key;
    // "A and B and C" -> "A et al."; "Last, First" -> "Last".
    std::vector<std::string> names;
    size_t k = 0;
    while (true) {
        size_t a = author.find(" and ", k);
        names.push_back(Trim(author.substr(k, a == std::string::npos ? std::string::npos : a - k)));
        if (a == std::string::npos) break;
        k = a + 5;
    }
    for (std::string &nm : names) {
        size_t comma = nm.find(',');
        if (comma != std::string::npos) {
            nm = Trim(nm.substr(0, comma));
        } else {
            size_t sp = nm.rfind(' ');
            if (sp != std::string::npos) nm = nm.substr(sp + 1);
        }
    }
    std::string who = names.size() == 1   ? names[0]
                      : names.size() == 2 ? names[0] + " and " + names[1]
                                          : names[0] + " et al.";
    std::string year = f.count("year") ? f.at("year") : "n.d.";
    return parenthetical ? "(" + who + ", " + year + ")" : who + " (" + year + ")";
}

namespace {
const std::vector<std::pair<std::string, std::uint32_t>> &NamedColors() {
    static const std::vector<std::pair<std::string, std::uint32_t>> kNamed = {
        {"red", 0xe06c75},    {"green", 0x98c379},  {"blue", 0x61afef},   {"yellow", 0xe5c07b},
        {"orange", 0xd19a66}, {"purple", 0xc678dd}, {"magenta", 0xc678dd}, {"cyan", 0x56b6c2},
        {"gray", 0x8b929e},   {"grey", 0x8b929e},   {"white", 0xffffff},  {"black", 0x000000},
        {"pink", 0xf5a3c7},   {"brown", 0xa0714f},  {"teal", 0x2aa198},   {"violet", 0xa78bfa},
        {"gold", 0xd4af37},   {"navy", 0x3b5bdb},   {"lime", 0xa3e635},   {"olive", 0x9a9a3a},
    };
    return kNamed;
}
}  // namespace

bool ParseColor(const std::string &name_in, std::uint32_t *rgb) {
    std::string name = Lower(Trim(name_in));
    if (!name.empty() && name[0] == '#') {
        std::string h = name.substr(1);
        if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
        if (h.size() != 6) return false;
        for (char c : h)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        *rgb = static_cast<std::uint32_t>(std::strtoul(h.c_str(), nullptr, 16));
        return true;
    }
    for (const auto &kv : NamedColors()) {
        if (kv.first == name) {
            *rgb = kv.second;
            return true;
        }
    }
    return false;
}

const std::vector<std::string> &ColorNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto &kv : NamedColors()) v.push_back(kv.first);
        return v;
    }();
    return names;
}

}  // namespace mepml

// ---------------------------------------------------------------------------
// HTML export

namespace mepml {

namespace {

std::string Esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string Slug(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? "section" : o;
}

std::string PlainText(const std::vector<Inline> &ins) {
    std::string o;
    for (const Inline &x : ins) {
        if (x.kind == InlineKind::Text || x.kind == InlineKind::Verbatim || x.kind == InlineKind::Math) o += x.text;
        else if (x.kind != InlineKind::Comment && x.kind != InlineKind::Footnote) o += PlainText(x.children);
    }
    return o;
}

struct HtmlWriter {
    const Document &doc;
    const HtmlOptions &opts;
    std::string out;
    std::vector<std::pair<int, std::string>> footnotes;  // number, html
    std::map<std::string, int> cite_numbers;
    bool in_slide = false;  // a <section class="slide"> is open

    // Closes the open slide's section, if there is one.
    void EndSlide() {
        if (in_slide) out += "</section>\n";
        in_slide = false;
    }

    std::string Inlines(const std::vector<Inline> &ins) {
        std::string o;
        for (const Inline &x : ins) o += Node(x);
        return o;
    }

    std::string Wrap(const char *tag, const Inline &x, const char *cls = nullptr) {
        std::string o = "<";
        o += tag;
        if (cls) o += std::string(" class=\"") + cls + "\"";
        o += ">" + Inlines(x.children) + "</" + tag + ">";
        return o;
    }

    std::string Node(const Inline &x) {
        switch (x.kind) {
            case InlineKind::Text: return Esc(x.text);
            case InlineKind::Bold: return Wrap("strong", x);
            case InlineKind::Italic: return Wrap("em", x);
            case InlineKind::Underline: return Wrap("u", x);
            case InlineKind::Superscript: return Wrap("sup", x);
            case InlineKind::Subscript: return Wrap("sub", x);
            case InlineKind::Small: return Wrap("small", x);
            case InlineKind::Big: return Wrap("span", x, "big");
            case InlineKind::Mono: return Wrap("span", x, "mono");
            case InlineKind::Highlight: return Wrap("mark", x);
            case InlineKind::Strike: return Wrap("s", x);
            case InlineKind::Insert: return Wrap("ins", x);
            case InlineKind::Delete: return Wrap("del", x);
            case InlineKind::Verbatim: return "<code>" + Esc(x.text) + "</code>";
            case InlineKind::Link: return "<a href=\"" + Esc(x.arg) + "\">" + Inlines(x.children) + "</a>";
            case InlineKind::Font:
                return "<span style=\"font-family:" + Esc(x.arg) + "\">" + Inlines(x.children) + "</span>";
            case InlineKind::FontSize:
                return "<span style=\"font-size:" + Esc(x.arg) + "pt\">" + Inlines(x.children) + "</span>";
            case InlineKind::Color: {
                std::uint32_t rgb = 0;
                std::string c = x.arg;
                if (ParseColor(x.arg, &rgb)) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "#%06x", rgb);
                    c = buf;
                }
                return "<span style=\"color:" + Esc(c) + "\">" + Inlines(x.children) + "</span>";
            }
            case InlineKind::Footnote: {
                footnotes.emplace_back(x.number, Inlines(x.children));
                std::string n = std::to_string(x.number);
                return "<sup class=\"fnref\" id=\"fnref" + n + "\"><a href=\"#fn" + n + "\">" + n + "</a></sup>";
            }
            case InlineKind::Cite:
            case InlineKind::CiteP: {
                std::string label = Esc(CiteLabel(doc, x.text, x.kind == InlineKind::CiteP));
                if (!doc.citations.count(x.text)) return "<span class=\"cite-missing\">" + label + "</span>";
                // data-key/data-kind let an HTML import rebuild the \cite.
                return "<a class=\"cite\" data-key=\"" + Esc(x.text) + "\" data-kind=\"" +
                       (x.kind == InlineKind::CiteP ? "citep" : "cite") + "\" href=\"#cite-" + Esc(Slug(x.text)) + "\">" +
                       label + "</a>";
            }
            case InlineKind::Math: {
                bool display = x.arg == "display";
                std::string o = std::string(display ? "\\[" : "\\(") + Esc(x.text) + (display ? "\\]" : "\\)");
                if (!x.alt.empty()) o = "<span class=\"math\" role=\"math\" aria-label=\"" + Esc(x.alt) + "\">" + o + "</span>";
                return o;
            }
            case InlineKind::Comment: return "";
        }
        return "";
    }

    std::string Caption(const Block &b) {
        if (b.caption_inlines.empty()) return "";
        return Inlines(b.caption_inlines);
    }

    void Block_(const Block &b, const std::string &label) {
        switch (b.kind) {
            case BlockKind::Paragraph: out += "<p>" + Inlines(b.inlines) + "</p>\n"; break;
            case BlockKind::Heading: {
                int lvl = std::min(6, b.level + 0);
                std::string t = PlainText(b.inlines);
                out += "<h" + std::to_string(lvl) + " id=\"" + Slug(t) + "\">" + Inlines(b.inlines) + "</h" +
                       std::to_string(lvl) + ">\n";
                break;
            }
            case BlockKind::Comment:
            case BlockKind::Meta:
            case BlockKind::Import:
            case BlockKind::Citation: break;
            case BlockKind::Callout: {
                std::string kw = Lower(b.keyword);
                out += "<div class=\"callout callout-" + kw + "\"><span class=\"callout-title\">" + Esc(b.keyword) +
                       "</span> " + Inlines(b.inlines) + "</div>\n";
                break;
            }
            case BlockKind::MathBlock: {
                std::string aria = b.alt.empty() ? "" : " role=\"math\" aria-label=\"" + Esc(b.alt) + "\"";
                out += "<div class=\"math-display\"" + aria + ">\\[" + Esc(b.code) + "\\]";
                if (!b.caption_inlines.empty()) out += "<div class=\"caption\">" + Caption(b) + "</div>";
                out += "</div>\n";
                break;
            }
            case BlockKind::Code: {
                bool show_code = true, show_results = true;
                CodeExports(doc, b, &show_code, &show_results);
                std::string fig;
                if (show_code) {
                    if (!b.lang.empty()) fig += "<figcaption class=\"lang\">" + Esc(b.lang) + "</figcaption>";
                    fig += "<pre><code class=\"language-" + Esc(b.lang) + "\">" + Esc(b.code) + "</code></pre>";
                }
                if (b.result_line_start >= 0 && show_results) {
                    // Text output as one block; figure lines as images.
                    std::string r;
                    bool any = false;
                    for (const std::string &line : b.result_lines) {
                        std::string img;
                        if (b.result_format.empty() && ResultImagePath(line, &img)) continue;
                        r += (any ? "\n" : "") + line;
                        any = true;
                    }
                    // HTML the block produced is the page's own markup;
                    // Markdown, the blocks that follow it.
                    if (b.result_format == "markdown") any = false;
                    if (any && b.result_format == "html") fig += "<div class=\"results results-html\">\n" + HtmlResultFragment(r) + "\n</div>";
                    else if (any) fig += "<pre class=\"results\">" + Esc(r) + "</pre>";
                }
                if (!fig.empty()) out += "<figure class=\"code\">" + fig + "</figure>\n";
                if (!b.result_images.empty() && show_results) {
                    // One figure for all of the block's plots, under one caption.
                    out += "<figure>";
                    for (const auto &img : b.result_images)
                        out += "<img src=\"" + Esc(img.second) + "\" alt=\"" + Esc(b.alt) + "\">";
                    if (!b.caption_inlines.empty())
                        out += "<figcaption>" + Esc(label) + ": " + Caption(b) + "</figcaption>";
                    out += "</figure>\n";
                } else if (!b.caption_inlines.empty() && !fig.empty()) {
                    out += "<p class=\"caption\">" + Caption(b) + "</p>\n";
                }
                break;
            }
            case BlockKind::Image: {
                out += "<figure><img src=\"" + Esc(b.value) + "\" alt=\"" + Esc(b.alt) + "\">";
                if (!b.caption_inlines.empty())
                    out += "<figcaption>" + Esc(label) + ": " + Caption(b) + "</figcaption>";
                out += "</figure>\n";
                break;
            }
            case BlockKind::Table: {
                // The wrapper scrolls a table wider than the text column
                // instead of letting it spill past the margin.
                out += "<div class=\"table-wrap\"><table>";
                if (!b.caption_inlines.empty())
                    out += "<caption>" + Esc(label) + ": " + Caption(b) + "</caption>";
                for (size_t r = 0; r < b.rows.size(); ++r) {
                    bool head = static_cast<int>(r) < b.header_rows;
                    out += "<tr>";
                    for (size_t c = 0; c < b.rows[r].size(); ++c) {
                        std::string al;
                        if (c < b.aligns.size()) {
                            Align a = b.aligns[c];
                            al = a == Align::Center ? " style=\"text-align:center\""
                                 : a == Align::Right ? " style=\"text-align:right\""
                                 : a == Align::Left  ? " style=\"text-align:left\""
                                                     : "";
                        }
                        const char *tag = head ? "th" : "td";
                        const TableCell &cell = b.rows[r][c];
                        const std::string body = cell.image.empty()
                                                     ? Inlines(cell.content)
                                                     : "<img src=\"" + Esc(cell.image) + "\" alt=\"\">";
                        out += std::string("<") + tag + al + ">" + body + "</" + tag + ">";
                    }
                    out += "</tr>";
                }
                out += "</table></div>\n";
                break;
            }
            case BlockKind::List: {
                // Nesting by indent.
                std::vector<std::pair<int, bool>> stack;  // indent, ordered
                for (const ListItem &it : b.items) {
                    while (!stack.empty() && it.indent < stack.back().first) {
                        out += stack.back().second ? "</li></ol>" : "</li></ul>";
                        stack.pop_back();
                    }
                    // Same depth but switching between bullets and numbers
                    // starts a new list rather than continuing this one.
                    if (!stack.empty() && it.indent == stack.back().first && it.ordered != stack.back().second) {
                        out += stack.back().second ? "</li></ol>" : "</li></ul>";
                        stack.pop_back();
                    }
                    if (stack.empty() || it.indent > stack.back().first) {
                        stack.emplace_back(it.indent, it.ordered);
                        out += it.ordered ? "<ol" + (it.number != 1 ? " start=\"" + std::to_string(it.number) + "\"" : std::string()) + "><li>"
                                          : std::string("<ul><li>");
                    } else {
                        out += "</li><li>";
                    }
                    if (it.checkbox >= 0)
                        out += std::string("<input type=\"checkbox\" disabled") + (it.checkbox ? " checked" : "") + "> ";
                    out += Inlines(it.content);
                }
                while (!stack.empty()) {
                    out += stack.back().second ? "</li></ol>" : "</li></ul>";
                    stack.pop_back();
                }
                out += "\n";
                break;
            }
            case BlockKind::Rule: out += "<hr>\n"; break;
            case BlockKind::Bibliography: {
                out += "<section class=\"bibliography\"><h2>References</h2><ol>";
                for (const std::string &key : doc.cite_order) {
                    const BibEntryParts e = BibEntry(doc.citations.at(key));
                    std::string entry = Esc(e.lead);
                    if (!e.title.empty()) entry += "<em>" + Esc(e.title) + "</em>";
                    entry += Esc(e.rest);
                    // The entry's fields ride along as data-bib-* attributes,
                    // so an HTML import gets the \citation back.
                    std::string data = " data-key=\"" + Esc(key) + "\"";
                    const Citation &cit = doc.citations.at(key);
                    for (const std::string &name : cit.field_order)
                        data += " data-bib-" + Esc(name) + "=\"" + Esc(cit.fields.at(name)) + "\"";
                    out += "<li id=\"cite-" + Esc(Slug(key)) + "\"" + data + ">" + entry + "</li>";
                }
                out += "</ol></section>\n";
                break;
            }
            case BlockKind::TableOfContents: {
                out += "<nav class=\"toc\"><h2>Contents</h2><ul>";
                for (const Block &h : doc.blocks) {
                    if (h.kind != BlockKind::Heading) continue;
                    std::string t = PlainText(h.inlines);
                    out += "<li class=\"toc-" + std::to_string(h.level) + "\"><a href=\"#" + Slug(t) + "\">" +
                           Inlines(h.inlines) + "</a></li>";
                }
                out += "</ul></nav>\n";
                break;
            }
            case BlockKind::Abstract: {
                // (ExportHtmlToLatex turns this section into \begin{abstract}.)
                out += "<section class=\"abstract\"><p class=\"abstract-title\">Abstract</p>";
                for (const std::vector<Inline> &para : AbstractParagraphs(b)) out += "<p>" + Inlines(para) + "</p>";
                out += "</section>\n";
                break;
            }
            case BlockKind::SlideBegin:
                // A slide left open ends where the next one starts.
                EndSlide();
                out += "<section class=\"slide\" id=\"slide-" + std::to_string(b.level) + "\">\n";
                in_slide = true;
                break;
            case BlockKind::SlideEnd: EndSlide(); break;
        }
    }
};

// The standalone page's default look: a readable serif column that nothing
// may widen (images scale down, wide tables and display math scroll inside
// it), a light theme (whatever the reader's system prefers -- a document
// reads like paper) with a dark one only on request (<html
// data-theme="dark">), and print rules. Colours are custom properties so a
// user stylesheet can retheme it by overriding :root.
// Never name the results-html class here: mepml_import.cpp finds code
// results by that text in the page (HtmlResultSources).
const char *kCss = R"css(
:root {
  --fg: #1f2328; --muted: #59636e; --bg: #fff; --link: #0b5cad; --rule: #d1d9e0; --rule-strong: #1f2328;
  --code-bg: #eff1f3; --pre-bg: #f6f8fa; --th-bg: #f6f8fa; --mark: #fff3a3; --results-bg: #fffbeb; --results-rule: #e5c07b;
  --cite: #0b5cad; --missing: #c62828; --ins: #2f7d32; --del: #c62828;
  color-scheme: light;
}
:root[data-theme="dark"] {
  --fg: #e6e6e6; --muted: #9ba3ad; --bg: #16181c; --link: #7cb7ff; --rule: #3a3f47; --rule-strong: #c9d1d9;
  --code-bg: #262a31; --pre-bg: #1e2227; --th-bg: #1e2227; --mark: #6b5a00; --results-bg: #262216; --results-rule: #9c7c2c;
  --cite: #7cb7ff; --missing: #ff7b72; --ins: #7ee787; --del: #ff7b72;
  color-scheme: dark;
}
*, *::before, *::after { box-sizing: border-box; }
html { -webkit-text-size-adjust: 100%; text-size-adjust: 100%; }
body { font-family: Charter, 'Bitstream Charter', 'Sitka Text', Cambria, Georgia, 'Times New Roman', serif; font-size: 1.0625rem;
  max-width: 46rem; margin: 0 auto; padding: 2.5rem 1.25rem 4rem; line-height: 1.6; color: var(--fg); background: var(--bg);
  overflow-wrap: break-word; font-kerning: normal; }
h1, h2, h3, h4, h5, h6 { font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif; line-height: 1.25; margin: 2em 0 .6em; scroll-margin-top: 1rem; text-wrap: balance; }
h1 { font-size: 1.7em; padding-bottom: .25em; border-bottom: 1px solid var(--rule); }
h2 { font-size: 1.35em; } h3 { font-size: 1.15em; } h4, h5, h6 { font-size: 1em; }
h1.title { font-size: 2.2em; text-align: center; border: 0; margin: 0 0 .8em; }
p { margin: 0 0 1em; }
a { color: var(--link); text-decoration-thickness: 1px; text-underline-offset: .15em; }
a.cite, a.footnote-ref, sup a { text-decoration: none; }
.cite-missing { color: var(--missing); }
ul, ol { padding-left: 1.6em; margin: 0 0 1em; } li { margin: .2em 0; } li > p { margin: 0; }
blockquote { margin: 1em 0; padding: 0 1em; color: var(--muted); border-left: 3px solid var(--rule); }
hr { border: 0; border-top: 1px solid var(--rule); margin: 2em 0; }
code, pre, kbd, .mono { font-family: 'JetBrains Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: .86em; }
code { background: var(--code-bg); padding: .1em .3em; border-radius: 4px; }
pre { background: var(--pre-bg); border: 1px solid var(--rule); padding: .8em 1em; margin: 0 0 1em; overflow-x: auto; border-radius: 6px; line-height: 1.45; tab-size: 4; }
pre code { background: none; padding: 0; font-size: 1em; border-radius: 0; }
pre.results { background: var(--results-bg); border-color: var(--results-rule); border-left-width: 3px; }
img, svg, video { max-width: 100%; height: auto; }
figure { margin: 1.8em 0; text-align: center; }
figure img { display: block; margin: 0 auto; }
figure img + img { margin-top: 1em; }
figure.code { text-align: left; margin: 1.2em 0; }
figure.code > pre:last-child { margin-bottom: 0; }
figure.code figcaption.lang { font: .72em system-ui, sans-serif; color: var(--muted); text-align: left; letter-spacing: .04em; text-transform: uppercase; margin: 0 0 .3em; max-width: none; }
figcaption, caption, .caption { font-size: .9em; color: var(--muted); text-align: center; line-height: 1.45; }
figcaption { margin: .7em auto 0; max-width: 38rem; }
.caption { margin: -.4em 0 1.2em; }
/* A table may use more than the text column, up to 60rem, centred on it. */
.table-wrap { width: min(100vw - 2.5rem, 60rem); max-width: none; margin: 1.8em 0 1.8em calc(50% - min(50vw - 1.25rem, 30rem)); overflow-x: auto; }
table { border-collapse: collapse; margin: 0 auto; font-size: .9em; line-height: 1.4; font-variant-numeric: lining-nums tabular-nums;
  border-top: 2px solid var(--rule-strong); border-bottom: 2px solid var(--rule-strong); }
caption { caption-side: top; padding-bottom: .6em; }
th, td { padding: .4em .75em; border-bottom: 1px solid var(--rule); vertical-align: top; }
th { font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif; font-size: .92em; font-weight: 600; background: var(--th-bg);
  border-bottom: 1px solid var(--rule-strong); }
tr:last-child > td { border-bottom: 0; }
td img, th img { display: block; max-height: 13em; width: auto; margin: 0 auto; }
figure.code > div.results { overflow-x: auto; }
.big { font-size: 1.3em; } mark { background: var(--mark); color: inherit; padding: 0 .1em; } ins { color: var(--ins); } del { color: var(--del); }
.callout { --c: #61afef; border-left: 4px solid var(--c); background: color-mix(in srgb, var(--c) 11%, transparent); padding: .7em 1em; margin: 1.2em 0; border-radius: 4px; }
.callout > :last-child { margin-bottom: 0; }
.callout-title { font: 600 .78em system-ui, sans-serif; letter-spacing: .06em; text-transform: uppercase; margin-bottom: .3em; }
.callout-warning, .callout-caution { --c: #e5a50a; }
.callout-error, .callout-danger { --c: #e06c75; }
.callout-tip, .callout-hint, .callout-success { --c: #98c379; }
.callout-todo, .callout-fixme, .callout-important { --c: #c678dd; }
.math-display { text-align: center; margin: 1.2em 0; max-width: 100%; overflow-x: auto; overflow-y: hidden; }
mjx-container[display="true"] { max-width: 100%; overflow-x: auto; overflow-y: hidden; }
.footnotes { font-size: .88em; color: var(--muted); border-top: 1px solid var(--rule); margin-top: 3em; padding-top: 1em; }
.footnotes ol { padding-left: 1.4em; }
.abstract { margin: 0 auto 2.5em; max-width: 38rem; font-size: .95em; padding: 1em 1.4em; border-top: 1px solid var(--rule); border-bottom: 1px solid var(--rule); }
.abstract p:last-child { margin-bottom: 0; }
.slide { margin: 1.5em 0; padding: .4em 1.4em 1em; border: 1px solid var(--rule); border-radius: 6px; }
.slide > :first-child { margin-top: .6em; }
.abstract-title { font: 600 .8em system-ui, sans-serif; text-align: center; letter-spacing: .1em; text-transform: uppercase; margin: 0 0 .6em; color: var(--muted); }
.toc { margin: 0 0 2em; } .toc ul { list-style: none; padding-left: 0; } .toc a { text-decoration: none; }
.toc-2 { padding-left: 1em; } .toc-3 { padding-left: 2em; } .toc-4 { padding-left: 3em; }
@media (max-width: 36rem) {
  body { font-size: 1rem; padding: 1.5rem 1rem 3rem; }
  h1.title { font-size: 1.8em; }
  .abstract { padding: .8em 0; }
  .table-wrap { width: calc(100vw - 2rem); margin-left: calc(50% - 50vw + 1rem); }
  pre { padding: .7em .8em; }
}
@media print {
  @page { margin: 2cm; }
  :root { --fg: #000; --bg: #fff; --link: #000; }
  body { max-width: none; padding: 0; font-size: 11pt; }
  a { text-decoration: none; }
  h1, h2, h3, h4 { break-after: avoid; }
  figure, table, pre, .math-display, .callout { break-inside: avoid; }
  .table-wrap, pre, .math-display { overflow: visible; }
  pre { white-space: pre-wrap; }
}
)css";

}  // namespace

std::string ToHtml(const Document &doc, const HtmlOptions &opts) {
    HtmlWriter w{doc, opts, {}, {}, {}, false};
    const std::vector<std::string> labels = BlockLabels(doc);
    const std::vector<bool> export_hidden = ExportHidden(doc);
    for (size_t i = 0; i < doc.blocks.size(); ++i)
        if (!export_hidden[i]) w.Block_(doc.blocks[i], labels[i]);
    w.EndSlide();
    if (!w.footnotes.empty()) {
        w.out += "<section class=\"footnotes\"><ol>";
        for (auto &fn : w.footnotes) {
            std::string n = std::to_string(fn.first);
            w.out += "<li id=\"fn" + n + "\">" + fn.second + " <a href=\"#fnref" + n + "\">&#8617;</a></li>";
        }
        w.out += "</ol></section>\n";
    }
    if (!opts.standalone) return w.out;
    std::string title = doc.title.empty() ? "Untitled" : doc.title;
    std::string html = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" + Esc(title) + "</title>\n";
    html += "<style>" + std::string(kCss) + "</style>\n";
    html += "<script>MathJax = { tex: { inlineMath: [['\\\\(', '\\\\)']] } };</script>\n";
    html += "<script async src=\"https://cdn.jsdelivr.net/npm/mathjax@4/tex-chtml.js\"></script>\n";
    html += "</head>\n<body>\n";
    if (!doc.title.empty()) html += "<h1 class=\"title\">" + Esc(doc.title) + "</h1>\n";
    html += w.out + "</body>\n</html>\n";
    return html;
}

namespace {

// The slideshow's own look, over kCss's content styles: 1280x720 slides
// (16:9) on a dark backdrop, the whole deck scaled to the window, one slide
// shown at a time; printing gives each its own page.
const char *kSlidesCss = R"css(
html, body { height: 100%; }
body { max-width: none; margin: 0; padding: 0; overflow: hidden; background: #1b1d22; font-size: 26px; line-height: 1.4; }
.deck { position: absolute; left: 50%; top: 50%; width: 1280px; height: 720px; transform: translate(-50%, -50%) scale(var(--scale, 1)); }
.slide { position: absolute; inset: 0; margin: 0; padding: 44px 64px 40px; border: 0; border-radius: 0; background: var(--bg);
  display: none; flex-direction: column; overflow: hidden; box-shadow: 0 10px 40px rgba(0,0,0,.45); }
.slide.current { display: flex; }
.slide > :first-child { margin-top: 0; }
.slide-title { font-size: 1.6em; margin: 0 0 .55em; padding-bottom: .2em; border-bottom: 3px solid var(--link); flex: none; }
.slide-body { flex: 1; min-height: 0; transform-origin: top left; }
.slide-body > :first-child { margin-top: 0; }
.slide-body h1, .slide-body h2, .slide-body h3, .slide-body h4 { margin: .6em 0 .3em; border: 0; font-size: 1.1em; }
.slide-body figure { margin: .6em 0; }
.slide-body figure img, .slide-body > p > img { max-height: 480px; width: auto; }
.slide-body .table-wrap { width: auto; margin: .6em 0; }
.slide-body pre { font-size: .72em; }
.slide-body .footnotes { margin-top: 1em; font-size: .6em; }
.title-slide { justify-content: center; align-items: center; text-align: center; }
.title-slide h1 { font-size: 2.3em; border: 0; margin: 0 0 .3em; }
.title-slide .subtitle { font-size: 1.2em; color: var(--muted); margin: 0 0 1.4em; }
.title-slide .author, .title-slide .date { margin: .2em 0; }
.slide-number { position: absolute; right: 26px; bottom: 16px; font: 16px system-ui, sans-serif; color: var(--muted); }
.progress { position: fixed; left: 0; bottom: 0; height: 4px; background: #6b8afd; transition: width .2s; }
.nav { position: fixed; left: 0; right: 0; bottom: 0; height: 96px; z-index: 10; display: flex; justify-content: center; align-items: center;
  gap: 18px; text-align: center; opacity: 0; transition: opacity .25s; background: linear-gradient(transparent, rgba(0,0,0,.35)); }
.nav:hover, .nav:focus-within { opacity: 1; }
.nav button { width: 52px; height: 52px; border: 0; border-radius: 50%; background: rgba(20,22,27,.8); color: #fff; font: bold 26px/1 system-ui, sans-serif;
  cursor: pointer; box-shadow: 0 2px 10px rgba(0,0,0,.4); }
.nav button:hover { background: #6b8afd; }
.nav button:disabled { opacity: .3; cursor: default; background: rgba(20,22,27,.8); }
.nav .count { min-width: 5em; text-align: center; font: 16px system-ui, sans-serif; color: #fff; text-shadow: 0 1px 3px #000; }
@media print {
  @page { size: 1280px 720px; margin: 0; }
  html, body { height: auto; overflow: visible; background: #fff; }
  .deck { position: static; transform: none; width: 1280px; height: auto; }
  .slide { position: relative; width: 1280px; height: 720px; display: flex; box-shadow: none; break-after: page; }
  .progress, .nav { display: none; }
}
)css";

// Stepping through the deck, and fitting it: the deck scales to the
// window, and a slide whose content runs past its bottom has that content
// scaled down until it fits (again once maths and pictures have loaded).
const char *kSlidesJs = R"js(
(function () {
  var slides = Array.prototype.slice.call(document.querySelectorAll('.deck > .slide'));
  var bar = document.querySelector('.progress');
  var prev = document.querySelector('.nav .prev'), next = document.querySelector('.nav .next');
  var count = document.querySelector('.nav .count');
  var cur = 0;
  function fitBody(s) {
    var b = s.querySelector('.slide-body');
    if (!b) return;
    b.style.transform = ''; b.style.width = '';
    var avail = b.clientHeight, need = b.scrollHeight;
    if (need > avail + 1 && avail > 0) {
      var k = Math.max(0.35, avail / need);
      b.style.transform = 'scale(' + k + ')';
      b.style.width = (100 / k) + '%';
    }
  }
  function fit() {
    var k = Math.min(window.innerWidth / 1280, window.innerHeight / 720);
    document.documentElement.style.setProperty('--scale', k);
    slides.forEach(fitBody);
  }
  function show(i) {
    cur = Math.max(0, Math.min(slides.length - 1, i));
    slides.forEach(function (s, j) { s.classList.toggle('current', j === cur); });
    if (bar) bar.style.width = (slides.length > 1 ? 100 * cur / (slides.length - 1) : 100) + '%';
    if (prev) prev.disabled = cur === 0;
    if (next) next.disabled = cur === slides.length - 1;
    if (count) count.textContent = (cur + 1) + ' / ' + slides.length;
    if (history.replaceState) history.replaceState(null, '', '#' + (cur + 1));
  }
  document.addEventListener('keydown', function (e) {
    if (e.altKey || e.ctrlKey || e.metaKey) return;
    var k = e.key;
    if (k === 'ArrowRight' || k === 'ArrowDown' || k === 'PageDown' || k === ' ' || k === 'n' || k === 'l' || k === '>') show(cur + 1);
    else if (k === 'ArrowLeft' || k === 'ArrowUp' || k === 'PageUp' || k === 'Backspace' || k === 'p' || k === 'h' || k === '<') show(cur - 1);
    else if (k === 'Home') show(0);
    else if (k === 'End') show(slides.length - 1);
    else if (k === 'f' && document.documentElement.requestFullscreen) document.documentElement.requestFullscreen();
    else return;
    e.preventDefault();
  });
  // The buttons in the bottom strip, which shows itself while the mouse
  // is over it; a click leaves no focus behind, so Space still steps.
  function step(d) { return function (e) { show(cur + d); e.currentTarget.blur(); }; }
  if (prev) prev.addEventListener('click', step(-1));
  if (next) next.addEventListener('click', step(1));
  document.addEventListener('click', function (e) {
    if (typeof e.clientX !== 'number' || e.target.closest('a, button, input, select, textarea, video, audio, .nav')) return;
    show(e.clientX > window.innerWidth / 2 ? cur + 1 : cur - 1);
  });
  var x0 = null;
  document.addEventListener('touchstart', function (e) { x0 = e.touches[0].clientX; }, {passive: true});
  document.addEventListener('touchend', function (e) {
    if (x0 === null) return;
    var dx = e.changedTouches[0].clientX - x0;
    if (Math.abs(dx) > 40) show(dx < 0 ? cur + 1 : cur - 1);
    x0 = null;
  });
  window.addEventListener('resize', fit);
  window.addEventListener('load', fit);
  window.addEventListener('beforeprint', function () { slides.forEach(function (s) { var b = s.querySelector('.slide-body'); if (b) { b.style.transform = ''; b.style.width = ''; } }); });
  window.addEventListener('afterprint', fit);
  if (window.MathJax && MathJax.startup && MathJax.startup.promise) MathJax.startup.promise.then(fit);
  else window.MathJax = Object.assign(window.MathJax || {}, {startup: {pageReady: function () { return MathJax.startup.defaultPageReady().then(fit); }}});
  fit();
  show((parseInt(location.hash.slice(1), 10) || 1) - 1);
})();
)js";

}  // namespace

std::vector<SlideHtml> SlideFragments(const Document &doc, const HtmlOptions &opts) {
    HtmlWriter w{doc, opts, {}, {}, {}, false};
    const std::vector<std::string> labels = BlockLabels(doc);
    const std::vector<bool> export_hidden = ExportHidden(doc);
    std::vector<SlideHtml> out;
    for (const Slide &sl : Slides(doc, 0)) {
        SlideHtml f;
        f.number = sl.number;
        w.out.clear();
        w.footnotes.clear();
        bool titled = false;
        for (size_t i = sl.first_block; i < sl.last_block; ++i) {
            const Block &b = doc.blocks[i];
            if (b.kind == BlockKind::SlideBegin || b.kind == BlockKind::SlideEnd || export_hidden[i]) continue;
            // The slide's first heading is its title, drawn at the top.
            if (!titled && b.kind == BlockKind::Heading) {
                f.title = w.Inlines(b.inlines);
                titled = true;
                continue;
            }
            w.Block_(b, labels[i]);
        }
        if (!w.footnotes.empty()) {
            w.out += "<section class=\"footnotes\"><ol>";
            for (auto &fn : w.footnotes)
                w.out += "<li value=\"" + std::to_string(fn.first) + "\">" + fn.second + "</li>";
            w.out += "</ol></section>\n";
        }
        f.body = w.out;
        out.push_back(std::move(f));
    }
    return out;
}

std::string ToSlidesHtml(const Document &doc, const HtmlOptions &opts) {
    const std::string subtitle = MetaValue(doc, "subtitle"), author = MetaValue(doc, "author"), date = MetaValue(doc, "date");
    std::string deck;
    int number = 0;
    if (!doc.title.empty() || !subtitle.empty() || !author.empty()) {
        ++number;
        deck += "<section class=\"slide title-slide\" id=\"slide-" + std::to_string(number) + "\">";
        if (!doc.title.empty()) deck += "<h1>" + Esc(doc.title) + "</h1>";
        if (!subtitle.empty()) deck += "<p class=\"subtitle\">" + Esc(subtitle) + "</p>";
        if (!author.empty()) deck += "<p class=\"author\">" + Esc(author) + "</p>";
        if (!date.empty()) deck += "<p class=\"date\">" + Esc(date) + "</p>";
        deck += "</section>\n";
    }
    for (const SlideHtml &f : SlideFragments(doc, opts)) {
        ++number;
        deck += "<section class=\"slide\" id=\"slide-" + std::to_string(number) + "\">";
        if (!f.title.empty()) deck += "<h2 class=\"slide-title\">" + f.title + "</h2>";
        deck += "<div class=\"slide-body\">\n" + f.body + "</div><div class=\"slide-number\">" + std::to_string(number) +
                "</div></section>\n";
    }
    if (!opts.standalone) return deck;
    const std::string page_title = doc.title.empty() ? "Slides" : doc.title;
    std::string html = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" + Esc(page_title) + "</title>\n";
    html += "<style>" + std::string(kCss) + kSlidesCss + "</style>\n";
    html += "<script>MathJax = { tex: { inlineMath: [['\\\\(', '\\\\)']] } };</script>\n";
    html += "<script async src=\"https://cdn.jsdelivr.net/npm/mathjax@4/tex-chtml.js\"></script>\n";
    html += "</head>\n<body>\n<main class=\"deck\">\n" + deck + "</main>\n<div class=\"progress\"></div>\n";
    html += "<nav class=\"nav\"><button class=\"prev\" title=\"Previous (h)\" aria-label=\"Previous slide\">&lt;</button>"
            "<span class=\"count\"></span>"
            "<button class=\"next\" title=\"Next (l)\" aria-label=\"Next slide\">&gt;</button></nav>\n";
    html += "<script>" + std::string(kSlidesJs) + "</script>\n</body>\n</html>\n";
    return html;
}

}  // namespace mepml

// ---------------------------------------------------------------------------
// Generated content: table of contents, bibliography

namespace mepml {

namespace {
int Cols(const std::string &s) {
    int n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}
// Byte offset of display column `col` in `s` (s.size() past the end).
size_t ColByte(const std::string &s, int col) {
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == col) return i;
            ++n;
        }
    }
    return s.size();
}
}  // namespace

std::string InlinePlainText(const std::vector<Inline> &ins) {
    std::string o;
    for (const Inline &x : ins) {
        if (x.kind == InlineKind::Text || x.kind == InlineKind::Verbatim || x.kind == InlineKind::Math) o += x.text;
        else if (x.kind != InlineKind::Comment && x.kind != InlineKind::Footnote) o += InlinePlainText(x.children);
    }
    return Trim(o);
}

BibEntryParts BibEntry(const Citation &c) {
    auto f = [&](const char *n) { return c.fields.count(n) ? c.fields.at(n) : std::string(); };
    BibEntryParts e;
    e.lead = f("author").empty() ? c.key : f("author");
    if (!f("year").empty()) e.lead += " (" + f("year") + ")";
    e.lead += ". ";
    e.title = f("title");
    for (const char *extra : {"journal", "booktitle", "publisher", "volume", "pages"})
        if (!f(extra).empty()) e.rest += ", " + f(extra);
    if (!f("doi").empty()) e.rest += ". doi:" + f("doi");
    if (!f("url").empty()) e.rest += ". " + f("url");
    e.rest += ".";
    if (e.title.empty()) {
        if (e.rest == ".") {
            // Nothing after the author and year: the lead's own full stop ends it.
            e.rest.clear();
            e.lead.pop_back();
        } else if (e.rest.rfind(", ", 0) == 0) {
            e.rest.erase(0, 2);
        }
    }
    return e;
}

std::vector<RenderedLine> RenderToc(const Document &doc, int width) {
    std::vector<RenderedLine> out;
    RenderedLine title;
    title.text = "Contents";
    title.spans.push_back({0, static_cast<int>(title.text.size()), kHeading | kBold, 2});
    out.push_back(title);
    bool any = false;
    for (const Block &b : doc.blocks) {
        if (b.kind != BlockKind::Heading || !b.origin.empty()) continue;
        any = true;
        RenderedLine l;
        const std::string indent(static_cast<size_t>(2 * (b.level - 1)), ' ');
        std::string name = InlinePlainText(b.inlines);
        if (name.empty()) name = "(untitled)";
        const int room = std::max(4, width - 2 - Cols(indent));
        if (Cols(name) > room) name = name.substr(0, ColByte(name, room - 3)) + "...";
        l.text = "  " + indent + name;
        const int from = static_cast<int>(2 + indent.size());
        l.spans.push_back({from, static_cast<int>(l.text.size()), kHeading | kLink, b.level});
        l.target_line = b.line_start;
        out.push_back(l);
    }
    if (!any) {
        RenderedLine l;
        l.text = "  (no headings yet)";
        l.spans.push_back({0, static_cast<int>(l.text.size()), kComment, 0});
        out.push_back(l);
    }
    return out;
}

std::vector<RenderedLine> RenderBibliography(const Document &doc, int width) {
    std::vector<RenderedLine> out;
    RenderedLine title;
    title.text = "References";
    title.spans.push_back({0, static_cast<int>(title.text.size()), kHeading | kBold, 2});
    out.push_back(title);
    if (doc.cite_order.empty()) {
        RenderedLine l;
        l.text = "  (nothing cited yet)";
        l.spans.push_back({0, static_cast<int>(l.text.size()), kComment, 0});
        out.push_back(l);
        return out;
    }
    int n = 0;
    for (const std::string &key : doc.cite_order) {
        const BibEntryParts e = BibEntry(doc.citations.at(key));
        const std::string num = "[" + std::to_string(++n) + "] ";
        // The entry as words, each tagged with whether it is title text,
        // then greedily filled into lines with a hanging indent.
        struct Word {
            std::string text;
            size_t italic;  // leading bytes of `text` set in italics
        };
        std::vector<Word> words;
        auto split = [&](const std::string &s, bool italic) {
            size_t k = 0;
            while (k < s.size()) {
                size_t e2 = s.find(' ', k);
                std::string w = s.substr(k, e2 == std::string::npos ? std::string::npos : e2 - k);
                if (!w.empty()) words.push_back({w, italic ? w.size() : 0});
                if (e2 == std::string::npos) break;
                k = e2 + 1;
            }
        };
        split(e.lead, false);
        split(e.title, true);
        // What follows the title (", The Computer Journal.") starts with
        // its punctuation, which stays glued to the title's last word
        // rather than beginning a line of its own.
        const size_t rest_at = words.size();
        split(e.rest, false);
        if (rest_at > 0 && rest_at < words.size() && !e.rest.empty() && e.rest[0] != ' ') {
            words[rest_at - 1].text += words[rest_at].text;
            words.erase(words.begin() + static_cast<long>(rest_at));
        }
        const std::string hang(static_cast<size_t>(Cols(num) + 2), ' ');
        RenderedLine line;
        line.text = "  " + num;
        line.spans.push_back({2, static_cast<int>(line.text.size()), kCite, 0});
        int line_cols = Cols(line.text);
        bool first_on_line = true;
        for (const Word &w : words) {
            const int need = Cols(w.text) + (first_on_line ? 0 : 1);
            const bool italic = w.italic > 0;
            if (!first_on_line && line_cols + need > width) {
                out.push_back(line);
                line = RenderedLine();
                line.text = hang;
                line_cols = Cols(hang);
                first_on_line = true;
            }
            if (!first_on_line) {
                // The space between two title words is title too, so the
                // italic run doesn't break at every gap.
                const size_t at = line.text.size();
                line.text += ' ';
                if (italic && !line.spans.empty() && (line.spans.back().style & kItalic) &&
                    line.spans.back().col_end == static_cast<int>(at))
                    line.spans.back().col_end = static_cast<int>(line.text.size());
                ++line_cols;
            }
            const int from = static_cast<int>(line.text.size());
            line.text += w.text;
            line_cols += Cols(w.text);
            if (italic) {
                const int to = from + static_cast<int>(w.italic);
                if (!line.spans.empty() && (line.spans.back().style & kItalic) && line.spans.back().col_end == from)
                    line.spans.back().col_end = to;
                else
                    line.spans.push_back({from, to, kItalic, 0});
            }
            first_on_line = false;
        }
        out.push_back(line);
    }
    return out;
}

namespace {
// A run of text in one style, and a word made of such runs (a word can
// change style midway: "*bold*," is two runs, one word).
struct StyledPiece {
    std::string text;
    std::uint32_t style = 0;
};
using StyledWord = std::vector<StyledPiece>;

// Inline content flattened to styled text, split into words at blanks.
void FlattenInlines(const Document &doc, const std::vector<Inline> &ins, std::uint32_t style,
                    std::vector<StyledWord> *words, bool *space_before) {
    auto emit = [&](const std::string &text, std::uint32_t st) {
        size_t k = 0;
        while (k < text.size()) {
            if (text[k] == ' ' || text[k] == '\t' || text[k] == '\n') {
                *space_before = true;
                ++k;
                continue;
            }
            size_t e = k;
            while (e < text.size() && text[e] != ' ' && text[e] != '\t' && text[e] != '\n') ++e;
            if (*space_before || words->empty()) words->emplace_back();
            *space_before = false;
            StyledWord &w = words->back();
            if (!w.empty() && w.back().style == st) w.back().text += text.substr(k, e - k);
            else w.push_back({text.substr(k, e - k), st});
            k = e;
        }
    };
    for (const Inline &x : ins) {
        const std::uint32_t st = style | FlagFor(x.kind);
        switch (x.kind) {
            case InlineKind::Text:
                emit(x.text, style);
                break;
            case InlineKind::Verbatim:
            case InlineKind::Math:
                emit(x.text, st);
                break;
            case InlineKind::Cite:
            case InlineKind::CiteP:
                emit(doc.citations.count(x.text) ? CiteLabel(doc, x.text, x.kind == InlineKind::CiteP) : "[?" + x.text + "]",
                     st);
                break;
            case InlineKind::Footnote:
                emit(SuperNumber(x.number), style);
                break;
            case InlineKind::Comment:
                break;
            default:
                FlattenInlines(doc, x.children, st, words, space_before);
                break;
        }
    }
}

// Words filled greedily into lines of at most `width` columns (a word
// longer than that gets a line of its own), each centred when `center`.
std::vector<RenderedLine> FillWords(const std::vector<StyledWord> &words, int width, bool center) {
    std::vector<RenderedLine> out;
    RenderedLine line;
    int cols = 0;
    auto word_cols = [](const StyledWord &w) {
        int c = 0;
        for (const StyledPiece &p : w) c += Cols(p.text);
        return c;
    };
    auto finish = [&] {
        if (line.text.empty()) return;
        if (center && cols < width) {
            const int pad = (width - cols) / 2;
            line.text.insert(0, static_cast<size_t>(pad), ' ');
            for (RenderedSpan &sp : line.spans) {
                sp.col_start += pad;
                sp.col_end += pad;
            }
        }
        out.push_back(std::move(line));
        line = RenderedLine();
        cols = 0;
    };
    for (const StyledWord &w : words) {
        const int wc = word_cols(w);
        if (cols > 0 && cols + 1 + wc > width) finish();
        if (cols > 0) {
            line.text += ' ';
            ++cols;
            // A space between two runs of one style belongs to them (an
            // underline or highlight stays unbroken across it).
            if (!line.spans.empty() && line.spans.back().col_end == static_cast<int>(line.text.size()) - 1 &&
                line.spans.back().style == w.front().style)
                line.spans.back().col_end++;
        }
        for (const StyledPiece &p : w) {
            const int from = static_cast<int>(line.text.size());
            line.text += p.text;
            const int to = static_cast<int>(line.text.size());
            if (p.style == 0) continue;
            if (!line.spans.empty() && line.spans.back().style == p.style && line.spans.back().col_end == from)
                line.spans.back().col_end = to;
            else
                line.spans.push_back({from, to, p.style, 0});
        }
        cols += wc;
    }
    finish();
    return out;
}
}  // namespace

std::vector<RenderedLine> RenderCaption(const Document &doc, const Block &b, const std::string &label, int width,
                                        bool center) {
    std::vector<StyledWord> words;
    bool space = true;
    if (!label.empty()) {
        // "Figure 1:" as one or two words of its own style.
        std::vector<Inline> lead(1);
        lead[0].text = label + ":";
        FlattenInlines(doc, lead, kDirective | kBold, &words, &space);
        space = true;
    }
    FlattenInlines(doc, b.caption_inlines, kItalic, &words, &space);
    return FillWords(words, std::max(8, width), center);
}

std::vector<RenderedLine> RenderAltText(const std::string &alt, int width, bool center) {
    std::vector<StyledWord> words;
    bool space = true;
    std::vector<Inline> text(1);
    text[0].text = alt;
    FlattenInlines(Document(), text, kComment | kItalic, &words, &space);
    return FillWords(words, std::max(8, width), center);
}

}  // namespace mepml

namespace mepml {
std::string HeadingSlug(const std::string &title) { return Slug(title); }
}  // namespace mepml
