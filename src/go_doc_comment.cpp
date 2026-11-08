// go/doc/comment port: see go_doc_comment.h.
#include "go_doc_comment.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "go_ast.h"

namespace godoc {

namespace {

using goast::DecodeLastRune;
using goast::DecodeRune;

bool HasPrefix(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool HasSuffix(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

// --- document model, reduced to what Printer.Comment prints ---

struct LinkDef {
    std::string text, url;
    bool used = false;
};

// A Text item: plain/italic text, or a link (auto or explicit) / doc link.
struct Text {
    enum Kind { Plain, Link, DocLink } kind = Plain;
    bool auto_ = false;
    std::string s;            // Plain
    std::vector<Text> text;   // Link / DocLink
};

struct Paragraph {
    std::vector<Text> text;
    std::string raw;  // first pass: the joined lines
};

struct ListItem {
    std::string number;
    std::vector<Paragraph> content;
};

struct Block {
    enum Kind { Para, Heading, Code, List } kind = Para;
    Paragraph para;     // Para; Heading text in para.text / raw
    std::string code;   // Code
    std::vector<ListItem> items;
    bool force_blank_before = false, force_blank_between = false;

    bool BlankBetween() const {
        if (force_blank_between) return true;
        for (const auto &it : items)
            if (it.content.size() != 1) return true;
        return false;
    }
    bool BlankBefore() const { return force_blank_before || BlankBetween(); }
};

const char *const kStdPkgs[] = {
    "bufio", "bytes", "cmp", "context", "crypto", "embed", "encoding", "errors", "expvar", "flag",
    "fmt", "hash", "html", "image", "io", "iter", "log", "maps", "math", "mime",
    "net", "os", "path", "plugin", "reflect", "regexp", "runtime", "slices", "sort", "strconv",
    "strings", "structs", "sync", "syscall", "testing", "time", "unicode", "unique", "unsafe", "weak",
};

bool IsStdPkg(std::string_view p) {
    return std::binary_search(std::begin(kStdPkgs), std::end(kStdPkgs), p,
                              [](std::string_view a, std::string_view b) { return a < b; });
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t i = s.find(sep, start);
        if (i == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, i - start));
        start = i + 1;
    }
    return out;
}

bool IsBlank(std::string_view s) { return s.empty() || (s.size() == 1 && s[0] == '\n'); }

std::string_view LeadingSpace(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(0, i);
}

std::string_view CommonPrefix(std::string_view a, std::string_view b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    return a.substr(0, i);
}

std::vector<std::string_view> Unindent(std::vector<std::string_view> lines) {
    size_t lo = 0, hi = lines.size();
    while (lo < hi && IsBlank(lines[lo])) lo++;
    while (hi > lo && IsBlank(lines[hi - 1])) hi--;
    if (lo == hi) return {};
    std::string_view prefix = LeadingSpace(lines[lo]);
    for (size_t i = lo + 1; i < hi; i++)
        if (!IsBlank(lines[i])) prefix = CommonPrefix(prefix, LeadingSpace(lines[i]));
    std::vector<std::string_view> out;
    for (size_t i = lo; i < hi; i++) {
        std::string_view line = lines[i];
        if (HasPrefix(line, prefix)) line.remove_prefix(prefix.size());
        if (TrimSpace(line).empty()) line = {};
        out.push_back(line);
    }
    size_t a = 0, b = out.size();
    while (a < b && out[a].empty()) a++;
    while (b > a && out[b - 1].empty()) b--;
    return std::vector<std::string_view>(out.begin() + static_cast<long>(a), out.begin() + static_cast<long>(b));
}

bool Indented(std::string_view line) { return !line.empty() && (line[0] == ' ' || line[0] == '\t'); }

bool ListMarker(std::string_view line, std::string_view *num, std::string_view *rest) {
    line = TrimSpace(line);
    if (line.empty()) return false;
    int n = 0;
    uint32_t r = DecodeRune(line, 0, &n);
    std::string_view nm, rs;
    if (r == 0x2022 || r == '*' || r == '+' || r == '-') {
        rs = line.substr(static_cast<size_t>(n));
    } else if (line[0] >= '0' && line[0] <= '9') {
        size_t k = 1;
        while (k < line.size() && line[k] >= '0' && line[k] <= '9') k++;
        if (k >= line.size() || (line[k] != '.' && line[k] != ')')) return false;
        nm = line.substr(0, k);
        rs = line.substr(k + 1);
    } else {
        return false;
    }
    if (!Indented(rs) || TrimSpace(rs).empty()) return false;
    if (num) *num = nm;
    if (rest) *rest = rs;
    return true;
}

bool IsList(std::string_view line) { return ListMarker(line, nullptr, nullptr); }

bool IsHeading(std::string_view line) {
    return line.size() >= 2 && line[0] == '#' && (line[1] == ' ' || line[1] == '\t') && TrimSpace(line) != "#";
}

bool IsOldHeading(std::string_view line, const std::vector<std::string_view> &all, size_t off) {
    if (off == 0 || !all[off - 1].empty() || off + 2 >= all.size() || !all[off + 1].empty() ||
        !LeadingSpace(all[off + 2]).empty())
        return false;
    line = TrimSpace(line);
    int w = 0;
    uint32_t r = DecodeRune(line, 0, &w);
    if (!goast::UnicodeIsLetter(r) || !goast::UnicodeIsUpper(r)) return false;
    r = DecodeLastRune(line, &w);
    if (!goast::UnicodeIsLetter(r) && !goast::UnicodeIsDigit(r)) return false;
    // strings.ContainsAny over runes: the set includes U+00B0 and U+00A7.
    {
        static const char32_t kBad[] = U";:!?+*/=[]{}_^°&§~%#@<\">\\";
        size_t i = 0;
        while (i < line.size()) {
            int ww = 0;
            uint32_t c = DecodeRune(line, i, &ww);
            for (const char32_t *p = kBad; *p; p++)
                if (static_cast<uint32_t>(*p) == c) return false;
            i += static_cast<size_t>(ww);
        }
    }
    for (std::string_view b = line;;) {
        size_t k = b.find('\'');
        if (k == std::string_view::npos) break;
        b = b.substr(k + 1);
        if (b != "s" && !HasPrefix(b, "s ")) return false;
    }
    for (std::string_view b = line;;) {
        size_t k = b.find('.');
        if (k == std::string_view::npos) break;
        b = b.substr(k + 1);
        if (b.empty() || HasPrefix(b, " ")) return false;
    }
    return true;
}

bool IsScheme(std::string_view s) {
    return s == "file" || s == "ftp" || s == "gopher" || s == "http" || s == "https" || s == "mailto" || s == "nntp";
}

bool InSet(unsigned char c, const char *extra, bool alnum) {
    if (c >= 0x80) return false;
    if (alnum && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return true;
    for (const char *p = extra; *p; p++)
        if (static_cast<unsigned char>(*p) == c) return true;
    return false;
}
bool IsHost(char c) { return InSet(static_cast<unsigned char>(c), "_@-.[]:", true); }
bool IsPunctB(char c) { return InSet(static_cast<unsigned char>(c), ".,:;?!", false); }
bool IsPath(char c) { return InSet(static_cast<unsigned char>(c), "$'()*+&#=@~_/-[]{}%", true); }
bool IsIdentASCII(char c) { return InSet(static_cast<unsigned char>(c), "_", true); }
bool ImportPathOK(char c) { return InSet(static_cast<unsigned char>(c), "-.~_+", true); }

bool AutoURL(std::string_view s, std::string_view *url) {
    size_t i;
    if (s.size() < 7) return false;
    if (s[3] == ':') i = 3;
    else if (s[4] == ':') i = 4;
    else if (s[5] == ':') i = 5;
    else if (s[6] == ':') i = 6;
    else return false;
    if (i + 3 > s.size() || s.substr(i, 3) != "://") return false;
    if (!IsScheme(s.substr(0, i))) return false;
    i += 3;
    if (i >= s.size() || !IsHost(s[i]) || IsPunctB(s[i])) return false;
    i++;
    size_t end = i;
    while (i < s.size() && IsHost(s[i])) {
        if (!IsPunctB(s[i])) end = i + 1;
        i++;
    }
    i = end;
    std::string stk;
    end = i;
    for (; i < s.size(); i++) {
        if (IsPunctB(s[i])) continue;
        if (!IsPath(s[i])) break;
        bool brk = false;
        switch (s[i]) {
        case '(': stk += ')'; break;
        case '{': stk += '}'; break;
        case '[': stk += ']'; break;
        case ')': case '}': case ']':
            if (stk.empty() || stk.back() != s[i]) brk = true;
            else stk.pop_back();
            break;
        default: break;
        }
        if (brk) break;
        if (stk.empty()) end = i + 1;
    }
    *url = s.substr(0, end);
    return true;
}

size_t Ident(std::string_view s) {
    size_t n = 0;
    while (n < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x80) {
            if (IsIdentASCII(static_cast<char>(c)) && (n > 0 || c < '0' || c > '9')) {
                n++;
                continue;
            }
            break;
        }
        int w = 0;
        uint32_t r = DecodeRune(s, n, &w);
        if (goast::UnicodeIsLetter(r)) {
            n += static_cast<size_t>(w);
            continue;
        }
        break;
    }
    return n;
}

bool IsName(std::string_view s) {
    size_t n = Ident(s);
    if (n == 0 || n != s.size()) return false;
    int w = 0;
    return goast::UnicodeIsUpper(DecodeRune(s, 0, &w));
}

bool ValidUTF8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        int w = 0;
        uint32_t r = DecodeRune(s, i, &w);
        if (r == goast::RuneError && w == 1) return false;
        i += static_cast<size_t>(w);
    }
    return true;
}

bool ValidImportPathElem(std::string_view e) {
    if (e.empty() || e[0] == '.' || e.back() == '.') return false;
    for (char c : e)
        if (!ImportPathOK(c)) return false;
    return true;
}

bool ValidImportPath(std::string_view p) {
    if (!ValidUTF8(p) || p.empty() || p[0] == '-' || p.find("//") != std::string_view::npos || p.back() == '/')
        return false;
    for (auto e : Split(p, '/'))
        if (!ValidImportPathElem(e)) return false;
    return true;
}

bool SplitDocName(std::string_view text, std::string_view *before, std::string_view *name) {
    size_t i = text.rfind('.');
    std::string_view nm = i == std::string_view::npos ? text : text.substr(i + 1);
    if (!IsName(nm)) {
        *before = text;
        *name = {};
        return false;
    }
    *before = i == std::string_view::npos ? std::string_view() : text.substr(0, i);
    *name = nm;
    return true;
}

bool ParseLink(std::string_view line, LinkDef *def) {
    if (line.empty() || line[0] != '[') return false;
    size_t i = line.find("]:");
    if (i == std::string_view::npos || i + 3 >= line.size() || (line[i + 2] != ' ' && line[i + 2] != '\t'))
        return false;
    std::string_view text = line.substr(1, i - 1);
    std::string_view url = TrimSpace(line.substr(i + 3));
    size_t j = url.find("://");
    if (j == std::string_view::npos || !IsScheme(url.substr(0, j))) return false;
    def->text = std::string(text);
    def->url = std::string(url);
    return true;
}

enum SpanKind { kCode = 1, kHeading, kList, kOldHeading, kPara };
struct Span {
    size_t start, end;
    SpanKind kind;
};

std::vector<Span> ParseSpans(const std::vector<std::string_view> &lines) {
    std::vector<Span> spans;
    long watchdog = 2 * static_cast<long>(lines.size());
    size_t i = 0, force_indent = 0;
    while (true) {
        while (i < lines.size() && lines[i].empty()) i++;
        if (i >= lines.size()) break;
        if (--watchdog < 0) break;  // go panics
        SpanKind kind;
        size_t start = i, end = i;
        if (i < force_indent || Indented(lines[i])) {
            bool unindented_list_ok = IsList(lines[i]) && i < force_indent;
            i++;
            while (i < lines.size() && (lines[i].empty() || i < force_indent || Indented(lines[i]) ||
                                        (unindented_list_ok && IsList(lines[i])))) {
                if (lines[i].empty()) unindented_list_ok = false;
                i++;
            }
            end = i;
            while (end > start && lines[end - 1].empty()) end--;
            if (end < lines.size() && HasPrefix(lines[end], "}")) end++;
            kind = IsList(lines[start]) ? kList : kCode;
        } else {
            i++;
            while (i < lines.size() && !lines[i].empty() && !Indented(lines[i])) i++;
            end = i;
            if (i < lines.size() && !lines[i].empty() && !IsList(lines[i])) {
                if (IsList(lines[i - 1])) {
                    force_indent = end;
                    end--;
                    while (end > start && IsList(lines[end - 1])) end--;
                } else if (HasSuffix(lines[i - 1], "{") || HasSuffix(lines[i - 1], "\\")) {
                    force_indent = end;
                    end--;
                }
                if (start == end && force_indent > start) {
                    i = start;
                    continue;
                }
            }
            if (end - start == 1 && IsHeading(lines[start])) kind = kHeading;
            else if (end - start == 1 && IsOldHeading(lines[start], lines, start)) kind = kOldHeading;
            else kind = kPara;
        }
        spans.push_back({start, end, kind});
        i = end;
    }
    return spans;
}

std::string Join(const std::vector<std::string_view> &v, size_t a, size_t b, std::string_view sep) {
    std::string out;
    for (size_t k = a; k < b; k++) {
        if (k > a) out += sep;
        out += v[k];
    }
    return out;
}

void AppendRune(std::string &s, uint32_t r) { goast::AppendRune(s, r); }

class ParseDoc {
public:
    std::vector<Block> content;
    std::vector<std::unique_ptr<LinkDef>> links_list;
    std::map<std::string, LinkDef *> links;

    // paragraph returns false when the lines were link definitions (added to the doc).
    bool Paragraph(const std::vector<std::string_view> &lines, size_t a, size_t b, struct Paragraph *out) {
        std::vector<LinkDef> defs;
        bool all = true;
        for (size_t k = a; k < b; k++) {
            LinkDef d;
            if (!ParseLink(lines[k], &d)) {
                all = false;
                break;
            }
            defs.push_back(d);
        }
        if (all) {
            for (auto &d : defs) {
                links_list.push_back(std::make_unique<LinkDef>(d));
                if (!links.count(d.text)) links[d.text] = links_list.back().get();
            }
            return false;
        }
        out->raw = Join(lines, a, b, "\n");
        return true;
    }

    Block List(const std::vector<std::string_view> &lines, size_t a, size_t b, bool force_blank_before) {
        std::string_view num;
        ListMarker(lines[a], &num, nullptr);
        Block list;
        list.kind = Block::List;
        list.force_blank_before = force_blank_before;
        ListItem *item = nullptr;
        std::vector<std::string_view> text;
        auto flush = [&] {
            if (item) {
                struct Paragraph p;
                if (Paragraph(text, 0, text.size(), &p)) item->content.push_back(std::move(p));
            }
            text.clear();
        };
        for (size_t k = a; k < b; k++) {
            std::string_view line = lines[k];
            std::string_view n, after;
            if (ListMarker(line, &n, &after) && n.empty() == num.empty()) {
                flush();
                list.items.emplace_back();
                list.items.back().number = std::string(n);
                item = &list.items.back();
                line = after;
            }
            line = TrimSpace(line);
            if (line.empty()) {
                list.force_blank_between = true;
                flush();
                continue;
            }
            text.push_back(TrimSpace(line));
        }
        flush();
        return list;
    }

    bool LookupPkg(std::string_view pkg) {
        if (pkg.find('/') != std::string_view::npos) return ValidImportPath(pkg);
        return IsStdPkg(pkg);
    }

    bool DocLink(std::string_view text, std::string_view before, std::string_view after) {
        if (!before.empty()) {
            int w = 0;
            uint32_t r = DecodeLastRune(before, &w);
            if (!goast::UnicodeIsPunct(r) && r != ' ' && r != '\t' && r != '\n') return false;
        }
        if (!after.empty()) {
            int w = 0;
            uint32_t r = DecodeRune(after, 0, &w);
            if (!goast::UnicodeIsPunct(r) && r != ' ' && r != '\t' && r != '\n') return false;
        }
        if (HasPrefix(text, "*")) text.remove_prefix(1);
        std::string_view pkg, name, recv;
        bool ok = SplitDocName(text, &pkg, &name);
        if (ok) SplitDocName(pkg, &pkg, &recv);
        if (!pkg.empty()) return LookupPkg(pkg);
        return false;  // lookupSym is always false for the default Parser
    }

    void ParseText(std::vector<Text> &out, std::string_view s, bool auto_link) {
        std::string w;
        size_t wrote = 0;
        auto write_until = [&](size_t i) {
            w.append(s.substr(wrote, i - wrote));
            wrote = i;
        };
        auto flush = [&](size_t i) {
            write_until(i);
            if (!w.empty()) {
                Text t;
                t.s = w;
                out.push_back(std::move(t));
                w.clear();
            }
        };
        for (size_t i = 0; i < s.size();) {
            std::string_view t = s.substr(i);
            if (auto_link) {
                std::string_view url;
                if (AutoURL(t, &url)) {
                    flush(i);
                    Text l;
                    l.kind = Text::Link;
                    l.auto_ = true;
                    Text p;
                    p.s = std::string(url);
                    l.text.push_back(p);
                    out.push_back(std::move(l));
                    i += url.size();
                    wrote = i;
                    continue;
                }
                if (size_t n = Ident(t); n > 0) {
                    i += n;  // no Words: never italicized
                    continue;
                }
            }
            if (HasPrefix(t, "``")) {
                if (t.size() >= 3 && t[2] == '`') {
                    // Go indexes t (= s[i:]) with the advanced i here; kept as is.
                    i += 3;
                    while (i < t.size() && t[i] == '`') i++;
                    continue;
                }
                write_until(i);
                AppendRune(w, 0x201C);
                i += 2;
                wrote = i;
            } else if (HasPrefix(t, "''")) {
                write_until(i);
                AppendRune(w, 0x201D);
                i += 2;
                wrote = i;
            } else {
                i++;
            }
        }
        flush(s.size());
    }

    std::vector<Text> ParseLinkedText(std::string_view text) {
        std::vector<Text> out;
        size_t wrote = 0;
        auto flush = [&](size_t i) {
            if (wrote < i) {
                ParseText(out, text.substr(wrote, i - wrote), true);
                wrote = i;
            }
        };
        long start = -1;
        std::string buf;
        for (size_t i = 0; i < text.size(); i++) {
            char c = text[i];
            if (c == '\n' || c == '\t') c = ' ';
            if (c == '[') {
                start = static_cast<long>(i);
            } else if (c == ']') {
                if (start >= 0) {
                    const size_t st = static_cast<size_t>(start);
                    auto it = links.find(buf);
                    if (it != links.end()) {
                        it->second->used = true;
                        flush(st);
                        Text l;
                        l.kind = Text::Link;
                        ParseText(l.text, text.substr(st + 1, i - st - 1), false);
                        out.push_back(std::move(l));
                        wrote = i + 1;
                    } else if (DocLink(text.substr(st + 1, i - st - 1), text.substr(0, st), text.substr(i + 1))) {
                        flush(st);
                        Text l;
                        l.kind = Text::DocLink;
                        ParseText(l.text, text.substr(st + 1, i - st - 1), false);
                        out.push_back(std::move(l));
                        wrote = i + 1;
                    }
                }
                start = -1;
                buf.clear();
            }
            if (start >= 0 && static_cast<long>(i) != start) buf += c;
        }
        flush(text.size());
        return out;
    }
};

void Indent(std::string &out, std::string_view indent, std::string_view s) {
    while (!s.empty()) {
        size_t k = s.find('\n');
        if (k == std::string_view::npos) {
            out += s;
            break;
        }
        out += s.substr(0, k);
        out += '\n';
        out += indent;
        s = s.substr(k + 1);
    }
}

void PrintText(std::string &out, std::string_view indent, const std::vector<Text> &x) {
    for (const Text &t : x) {
        switch (t.kind) {
        case Text::Plain: Indent(out, indent, t.s); break;
        case Text::Link:
            if (t.auto_) {
                PrintText(out, indent, t.text);
            } else {
                out += '[';
                PrintText(out, indent, t.text);
                out += ']';
            }
            break;
        case Text::DocLink:
            out += '[';
            PrintText(out, indent, t.text);
            out += ']';
            break;
        }
    }
}

}  // namespace

std::string_view TrimSpace(std::string_view s) {
    size_t a = 0;
    while (a < s.size()) {
        int w = 0;
        uint32_t r = DecodeRune(s, a, &w);
        if (r == goast::RuneError && w == 1) break;
        if (!goast::UnicodeIsSpace(r)) break;
        a += static_cast<size_t>(w);
    }
    size_t b = s.size();
    while (b > a) {
        int w = 0;
        uint32_t r = DecodeLastRune(s.substr(a, b - a), &w);
        if (r == goast::RuneError && w == 1) break;
        if (!goast::UnicodeIsSpace(r)) break;
        b -= static_cast<size_t>(w);
    }
    return s.substr(a, b - a);
}

std::string ReformatComment(std::string_view text) {
    const std::vector<std::string_view> lines = Unindent(Split(text, '\n'));
    ParseDoc d;
    size_t prev_end = 0;
    bool have_prev = false;
    for (const Span &s : ParseSpans(lines)) {
        Block b;
        bool add = true;
        switch (s.kind) {
        case kList: b = d.List(lines, s.start, s.end, (have_prev ? prev_end : 0) < s.start); break;
        case kCode: {
            std::vector<std::string_view> sub(lines.begin() + static_cast<long>(s.start),
                                              lines.begin() + static_cast<long>(s.end));
            auto body = Unindent(sub);
            b.kind = Block::Code;
            for (auto l : body) {
                b.code += l;
                b.code += '\n';
            }
            break;
        }
        case kOldHeading:
            b.kind = Block::Heading;
            b.para.raw = std::string(TrimSpace(lines[s.start]));
            break;
        case kHeading:
            b.kind = Block::Heading;
            b.para.raw = std::string(TrimSpace(lines[s.start].substr(1)));
            break;
        case kPara:
            add = d.Paragraph(lines, s.start, s.end, &b.para);
            break;
        }
        if (add) d.content.push_back(std::move(b));
        prev_end = s.end;
        have_prev = true;
    }
    for (Block &b : d.content) {
        if (b.kind == Block::Para) {
            b.para.text = d.ParseLinkedText(b.para.raw);
        } else if (b.kind == Block::List) {
            for (auto &it : b.items)
                for (auto &p : it.content) p.text = d.ParseLinkedText(p.raw);
        } else if (b.kind == Block::Heading) {
            Text t;
            t.s = b.para.raw;
            b.para.text.push_back(t);
        }
    }

    std::string out;
    for (size_t i = 0; i < d.content.size(); i++) {
        const Block &x = d.content[i];
        if (i > 0 && (x.kind != Block::List || x.BlankBefore())) out += '\n';
        switch (x.kind) {
        case Block::Para:
            PrintText(out, "", x.para.text);
            out += '\n';
            break;
        case Block::Heading:
            out += "# ";
            PrintText(out, "", x.para.text);
            out += '\n';
            break;
        case Block::Code: {
            std::string_view md = x.code;
            while (!md.empty()) {
                size_t k = md.find('\n');
                std::string_view line = md.substr(0, k);
                md = k == std::string_view::npos ? std::string_view() : md.substr(k + 1);
                if (!line.empty()) {
                    out += '\t';
                    out += line;
                }
                out += '\n';
            }
            break;
        }
        case Block::List: {
            const bool loose = x.BlankBetween();
            for (size_t k = 0; k < x.items.size(); k++) {
                const ListItem &item = x.items[k];
                if (k > 0 && loose) out += '\n';
                out += ' ';
                if (item.number.empty()) {
                    out += " - ";
                } else {
                    out += item.number;
                    out += ". ";
                }
                for (size_t j = 0; j < item.content.size(); j++) {
                    if (j > 0) out += "\n    ";
                    PrintText(out, "    ", item.content[j].text);
                    out += '\n';
                }
            }
            break;
        }
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        const bool used = pass == 0;
        bool first = true;
        for (const auto &def : d.links_list) {
            if (def->used != used) continue;
            if (first) {
                out += '\n';
                first = false;
            }
            out += '[';
            out += def->text;
            out += "]: ";
            out += def->url;
            out += '\n';
        }
    }
    return out;
}

}  // namespace godoc
