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

// Reads a `{...}` group starting at s[i] == '{', balancing nested braces and
// honouring `\{` / `\}` escapes. Returns the index just past the closing
// brace (content is [i+1, ret-1)), or -1 if unbalanced before `limit`.
int ReadGroup(const std::string &s, int i, int limit) {
    if (At(s, i) != '{') return -1;
    int depth = 0;
    for (int j = i; j < limit; ++j) {
        char c = s[static_cast<size_t>(j)];
        if (c == '\\') {
            ++j;
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}' && --depth == 0) return j + 1;
    }
    return -1;
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
            ++j;
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
    if (StartsAt(s, node->end, "@alttext{")) {
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
    if (ngroups == 1) {
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
            else if (i + 1 < e && !IsSpace(At(s, i + 1))) {
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
// `// result_begin:` optionally followed by the results' kind (`html`).
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
    if (format) *format = Lower(rest);
    return true;
}
bool IsResultEnd(const std::string &line) {
    std::string t = Trim(line);
    return t == "// result_end" || t == "// result_end:" || t == "//result_end";
}

// `@name` at the start of a trimmed line.
std::string DirectiveName(const std::string &line) {
    int i = Indent(line);
    if (At(line, i) != '@') return "";
    int j = i + 1;
    while (IsAlpha(At(line, j))) ++j;
    return Sub(line, i + 1, j);
}

bool IsKnownDirective(const std::string &name) {
    return name == "import" || name == "citation" || name == "image" || name == "caption" || name == "alttext" ||
           name == "bibliography" || name == "printbibliography" || name == "toc" || IsBibtexType(name);
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

    // Blocks a caption/alttext line may attach to.
    bool Attach(int i, const std::string &name) {
        if (doc.blocks.empty()) return false;
        Block &b = doc.blocks.back();
        if (b.line_end != i - 1) return false;
        if (b.kind != BlockKind::Image && b.kind != BlockKind::Table && b.kind != BlockKind::MathBlock &&
            b.kind != BlockKind::Code)
            return false;
        int col = Indent(L(i)) + 1 + Len(name);
        int after = -1;
        std::string arg = OneLineGroup(i, col, &after);
        if (after < 0) {
            Diag(Diagnostic::Error, i, 0, Len(L(i)), "unterminated @" + name + "{...}");
            return false;
        }
        CheckTrailing(i, after);
        b.line_end = i;
        SetText(b);
        int off = b.line_offsets.back();
        if (name == "caption") {
            b.caption = Trim(arg);
            b.caption_line = i;
            b.caption_inlines = Inlines(b, off + col + 1, off + after - 1);
        } else {
            b.alt = Trim(arg);
            b.alt_line = i;
        }
        return true;
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

    int ParseDirective(int i, const std::string &name) {
        const std::string &s = L(i);
        int col = Indent(s) + 1 + Len(name);
        if (name == "caption" || name == "alttext") {
            if (!Attach(i, name)) {
                Diag(Diagnostic::Warning, i, 0, Len(s),
                     "@" + name + " must directly follow an image, table, code block or display math");
                doc.blocks.push_back(MakeBlock(BlockKind::Comment, i, i));
            }
            return i + 1;
        }
        if (name == "printbibliography")
            Diag(Diagnostic::Info, i, Indent(s), col, "@printbibliography is now @bibliography");
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
                Diag(Diagnostic::Error, i, 0, Len(s), "unterminated @" + name + "{...}");
                doc.blocks.push_back(MakeBlock(BlockKind::Paragraph, i, i));
                return i + 1;
            }
            CheckTrailing(i, after);
            Block b = MakeBlock(name == "import" ? BlockKind::Import : BlockKind::Image, i, i);
            b.value = Trim(arg);
            if (b.value.empty()) Diag(Diagnostic::Error, i, 0, Len(s), "@" + name + " needs a path");
            doc.blocks.push_back(std::move(b));
            return i + 1;
        }
        // @citation{key}{fields}  or  bibtex  @article{key, fields}
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
            if (name != "citation") break;
            int k = g1;
            while (k < Len(joined) && IsSpace(joined[static_cast<size_t>(k)])) ++k;
            if (k >= Len(joined)) continue;
            g2 = ReadGroup(joined, k, Len(joined));
            if (g2 >= 0) break;
        }
        if (j >= n || j >= i + 500 || g1 < 0 || (name == "citation" && g2 < 0)) {
            Diag(Diagnostic::Error, i, 0, Len(s), "unterminated @" + name + " entry");
            doc.blocks.push_back(MakeBlock(BlockKind::Paragraph, i, i));
            return i + 1;
        }
        Block b = MakeBlock(BlockKind::Citation, i, j);
        std::string body;
        if (name == "citation") {
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

    int ParseTable(int i) {
        int j = i;
        while (j < n && IsTableRow(Trim(L(j)))) ++j;
        Block b = MakeBlock(BlockKind::Table, i, j - 1);
        for (int k = i; k < j; ++k) {
            const std::string &s = L(k);
            int off = b.line_offsets[static_cast<size_t>(k - i)];
            // Split on unescaped bars outside `verbatim`.
            std::vector<std::pair<int, int>> cells;
            int first = static_cast<int>(s.find('|'));
            int last = static_cast<int>(s.rfind('|'));
            int cb = first + 1;
            bool tick = false;
            for (int c = first + 1; c <= last; ++c) {
                char ch = s[static_cast<size_t>(c)];
                if (ch == '\\') {
                    ++c;
                    continue;
                }
                if (ch == '`') tick = !tick;
                if (ch == '|' && !tick) {
                    cells.emplace_back(cb, c);
                    cb = c + 1;
                }
            }
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
            if (Trim(s).empty()) break;
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
        if (HeadingLevel(s) || IsFence(t) || IsComment(s) || IsRule(t) || IsTableRow(t) || IsListItem(s)) return true;
        if (t.rfind("$$", 0) == 0 || t.rfind("\\[", 0) == 0) return true;
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
            if (t.empty()) {
                ++i;
                continue;
            }
            if (IsMetaLine(s)) {
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
            } else if (IsTableRow(t)) {
                i = ParseTable(i);
            } else if (IsListItem(s)) {
                i = ParseList(i);
            } else {
                std::string d = DirectiveName(s);
                if (!d.empty() && IsKnownDirective(d)) {
                    i = ParseDirective(i, d);
                } else {
                    if (!d.empty())
                        Diag(Diagnostic::Warning, i, Indent(s), Indent(s) + 1 + Len(d),
                             "unknown directive @" + d);
                    i = ParseParagraph(i);
                }
            }
        }
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

bool ResultImagePath(const std::string &text, std::string *path) {
    std::string t = Trim(text);
    if (t.rfind("@image{", 0) != 0 || t.back() != '}') return false;
    *path = Trim(t.substr(7, t.size() - 8));
    return !path->empty();
}

std::vector<std::string> BlockLabels(const Document &doc) {
    std::vector<std::string> out(doc.blocks.size());
    int figures = 0, tables = 0;
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block &b = doc.blocks[i];
        if (b.kind == BlockKind::Image || (b.kind == BlockKind::Code && !b.result_images.empty()))
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
        // `@import{path}` in the body, or `//? Import: path` in the header:
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
        if ((n == "results" || n == "output") && Lower(Trim(o.value.s)).find("html") != std::string::npos) return "html";
    }
    return "";
}

std::vector<std::string> FormatResults(const std::string &output, const std::string &format) {
    std::vector<std::string> out;
    out.push_back(format.empty() ? "// result_begin:" : "// result_begin: " + format);
    std::string o = output;
    while (!o.empty() && (o.back() == '\n' || o.back() == '\r')) o.pop_back();
    size_t k = 0;
    if (!o.empty()) {
        while (true) {
            size_t e = o.find('\n', k);
            std::string ln = o.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            out.push_back(ln.empty() ? "//" : "// " + ln);
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

    // `@name{arg}` directive line: name+braces markup, arg styled.
    void Directive(int line, const std::string &target) {
        std::string s = LineText(line);
        int at = static_cast<int>(s.find('@'));
        int brace = static_cast<int>(s.find('{', static_cast<size_t>(at < 0 ? 0 : at)));
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
                if (blk.caption_line >= 0) {
                    Directive(blk.caption_line, "");
                    // Caption text itself gets inline styling on top of the directive colour.
                    std::string s = LineText(blk.caption_line);
                    int brace = static_cast<int>(s.find('{'));
                    int g = ReadGroup(s, brace, static_cast<int>(s.size()));
                    if (brace >= 0 && g > 0) {
                        out.erase(std::remove_if(out.begin(), out.end(),
                                                 [&](const Span &sp) {
                                                     return sp.line == blk.caption_line &&
                                                            (sp.style & kDirective) && sp.col_start <= brace;
                                                 }),
                                  out.end());
                        Span d;
                        d.style = kDirective;
                        d.markup = true;
                        d.replace = "Caption: ";
                        Line(blk.caption_line, static_cast<int>(s.find('@')), brace + 1, d);
                        Span cap;
                        cap.style = kItalic;
                        Inlines(blk.caption_inlines, cap);
                        Span close = d;
                        close.replace.clear();
                        Line(blk.caption_line, g - 1, g, close);
                    }
                }
                if (blk.alt_line >= 0) Directive(blk.alt_line, "");
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
            for (const TableCell &c : row) Inlines(c.content, cell);
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
            for (int line = blk.result_line_start + 1; line < blk.result_line_end; ++line) {
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
                out += "<figure class=\"code\">";
                if (!b.lang.empty()) out += "<figcaption class=\"lang\">" + Esc(b.lang) + "</figcaption>";
                out += "<pre><code class=\"language-" + Esc(b.lang) + "\">" + Esc(b.code) + "</code></pre>";
                if (b.result_line_start >= 0) {
                    // Text output as one block; figure lines as images.
                    std::string r;
                    bool any = false;
                    for (const std::string &line : b.result_lines) {
                        std::string img;
                        if (b.result_format.empty() && ResultImagePath(line, &img)) continue;
                        r += (any ? "\n" : "") + line;
                        any = true;
                    }
                    // HTML the block produced is the page's own markup.
                    if (any && b.result_format == "html") out += "<div class=\"results results-html\">\n" + HtmlResultFragment(r) + "\n</div>";
                    else if (any) out += "<pre class=\"results\">" + Esc(r) + "</pre>";
                }
                out += "</figure>\n";
                if (!b.result_images.empty()) {
                    for (const auto &img : b.result_images)
                        out += "<figure><img src=\"" + Esc(img.second) + "\" alt=\"" + Esc(b.alt) + "\">";
                    if (!b.caption_inlines.empty())
                        out += "<figcaption>" + Esc(label) + ": " + Caption(b) + "</figcaption>";
                    out += "</figure>\n";
                } else if (!b.caption_inlines.empty()) {
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
                out += "<table>";
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
                        out += std::string("<") + tag + al + ">" + Inlines(b.rows[r][c].content) + "</" + tag + ">";
                    }
                    out += "</tr>";
                }
                out += "</table>\n";
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
                    // so an HTML import gets the @citation back.
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
        }
    }
};

const char *kCss = R"css(
body { font-family: Georgia, 'Times New Roman', serif; max-width: 46rem; margin: 2rem auto; padding: 0 1rem; line-height: 1.55; color: #1f2328; background: #fff; }
h1, h2, h3, h4 { font-family: system-ui, sans-serif; line-height: 1.25; }
code, pre, .mono { font-family: 'JetBrains Mono', ui-monospace, monospace; font-size: 0.92em; }
code { background: #f2f2f2; padding: 0 .25em; border-radius: 3px; }
pre { background: #f6f8fa; padding: .8em 1em; overflow-x: auto; border-radius: 6px; }
pre code { background: none; padding: 0; }
pre.results { background: #fffbe6; border-left: 3px solid #e5c07b; }
figure { margin: 1.2em 0; } figure.code figcaption.lang { font: 0.75em system-ui, sans-serif; color: #888; text-align: left; font-style: normal; margin: 0 0 -.3em; }
figcaption, caption, .caption { font-style: italic; color: #555; text-align: center; margin: .3em 0; }
img { max-width: 100%; }
table { border-collapse: collapse; margin: 1em auto; } th, td { border: 1px solid #ccc; padding: .3em .7em; } th { background: #f2f2f2; }
.big { font-size: 1.3em; } mark { background: #fff3a3; } ins { color: #2f7d32; } del { color: #c62828; }
.callout { border-left: 4px solid #61afef; background: #eef6fd; padding: .6em .9em; margin: 1em 0; border-radius: 4px; }
.callout-title { font: bold 0.8em system-ui, sans-serif; letter-spacing: .05em; }
.callout-warning, .callout-caution { border-color: #e5a50a; background: #fdf6e3; }
.callout-error, .callout-danger { border-color: #e06c75; background: #fdeeee; }
.callout-tip, .callout-hint, .callout-success { border-color: #98c379; background: #f0f8ec; }
.callout-todo, .callout-fixme, .callout-important { border-color: #c678dd; background: #f7effb; }
.math-display { text-align: center; margin: 1em 0; overflow-x: auto; }
.footnotes { font-size: .9em; border-top: 1px solid #ddd; margin-top: 2em; }
.cite-missing { color: #c62828; }
.toc ul { list-style: none; padding-left: 0; } .toc-2 { padding-left: 1em; } .toc-3 { padding-left: 2em; } .toc-4 { padding-left: 3em; }
)css";

}  // namespace

std::string ToHtml(const Document &doc, const HtmlOptions &opts) {
    HtmlWriter w{doc, opts, {}, {}, {}};
    const std::vector<std::string> labels = BlockLabels(doc);
    for (size_t i = 0; i < doc.blocks.size(); ++i) w.Block_(doc.blocks[i], labels[i]);
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
    std::string html = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + Esc(title) + "</title>\n";
    html += "<style>" + std::string(kCss) + "</style>\n";
    html += "<script>MathJax = { tex: { inlineMath: [['\\\\(', '\\\\)']] } };</script>\n";
    html += "<script async src=\"https://cdn.jsdelivr.net/npm/mathjax@3/es5/tex-chtml.js\"></script>\n";
    html += "</head>\n<body>\n";
    if (!doc.title.empty()) html += "<h1 class=\"title\">" + Esc(doc.title) + "</h1>\n";
    html += w.out + "</body>\n</html>\n";
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

}  // namespace mepml

namespace mepml {
std::string HeadingSlug(const std::string &title) { return Slug(title); }
}  // namespace mepml
