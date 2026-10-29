// mep's mepml formatter -- see mepml_format.h for what it does and why it
// can't change what a document says.
#include "mepml_format.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mepml {
namespace {

using Lines = std::vector<std::string>;

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Characters, not bytes: a UTF-8 continuation byte adds nothing.
int Cols(const std::string &s) {
    int n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

std::string RTrim(const std::string &s) {
    size_t e = s.size();
    while (e > 0 && IsBlank(s[e - 1])) --e;
    return s.substr(0, e);
}

std::string Trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && IsBlank(s[b])) ++b;
    return RTrim(s.substr(b));
}

std::string LeadingSpace(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(0, i);
}

// The same width in blanks (tabs kept, so the columns still line up).
std::string Blanked(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\t') out += '\t';
        else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) out += ' ';
    }
    return out;
}

Lines SplitLines(const std::string &text) {
    Lines out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) {
            if (at < text.size()) out.push_back(text.substr(at));
            break;
        }
        out.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    for (std::string &l : out)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    return out;
}

std::string Join(const Lines &lines, size_t first, size_t last) {
    std::string s;
    for (size_t i = first; i <= last; ++i) {
        if (i > first) s += '\n';
        s += lines[i];
    }
    return s;
}

// --- the signature the self-check compares -----------------------------

// Whitespace runs as one space, none at either end.
std::string Norm(const std::string &s) {
    std::string out;
    bool gap = false;
    for (char c : s) {
        if (IsBlank(c)) {
            gap = !out.empty();
            continue;
        }
        if (gap) out += ' ';
        gap = false;
        out += c;
    }
    return out;
}

void InlineSig(const std::vector<Inline> &ins, std::string *out) {
    // Neighbouring text runs merge first: where a parse splits plain text
    // into runs need not survive a refill, and doesn't matter.
    std::string text;
    auto flush = [&] {
        if (text.empty()) return;
        *out += "T{" + Norm(text) + "}";
        text.clear();
    };
    for (const Inline &x : ins) {
        if (x.kind == InlineKind::Text) {
            text += x.text;
            continue;
        }
        if (!text.empty() && IsBlank(text.back())) text.back() = ' ';
        flush();
        *out += "<" + std::to_string(static_cast<int>(x.kind)) + "|" + Norm(x.arg) + "|" + Norm(x.alt) + "|" +
                Norm(x.text) + "|" + std::to_string(x.number) + "|";
        InlineSig(x.children, out);
        *out += ">";
    }
    flush();
}

std::string InlineSig(const std::vector<Inline> &ins) {
    std::string s;
    InlineSig(ins, &s);
    // A space either side of a construct is the same space however the
    // lines fell.
    return Norm(s);
}

// A comment's words, without its `//` leaders.
std::string CommentWords(const std::string &text) {
    std::string words;
    for (const std::string &l : SplitLines(text)) {
        const size_t p = l.find("//");
        words += ' ';
        words += p == std::string::npos ? l : l.substr(p + 2);
    }
    return Norm(words);
}

std::string BlockSig(const Block &b) {
    std::string s = "B" + std::to_string(static_cast<int>(b.kind)) + "|" + std::to_string(b.level) + "|" + b.keyword +
                    "|" + Norm(b.value) + "|" + b.lang + "|";
    s += b.kind == BlockKind::Code ? b.code : Norm(b.code);
    s += "|" + b.include + "|" + b.result_format + "|";
    for (const Option &o : b.options) s += o.name + "=" + o.value.s + ";";
    for (const std::string &r : b.result_lines) s += "R{" + r + "}";
    for (const auto &im : b.result_images) s += "I{" + im.second + "}";
    s += "|C{" + Norm(b.caption) + "}A{" + Norm(b.alt) + "}" + InlineSig(b.caption_inlines);
    s += "|" + InlineSig(b.inlines);
    for (const auto &row : b.rows) {
        s += "[";
        for (const TableCell &c : row) s += "{" + InlineSig(c.content) + "|" + c.image + "}";
        s += "]";
    }
    for (Align a : b.aligns) s += std::to_string(static_cast<int>(a));
    s += "|h" + std::to_string(b.header_rows);
    for (const ListItem &it : b.items)
        s += "{" + std::to_string(it.indent) + (it.ordered ? "o" : "u") + std::to_string(it.number) + "c" +
             std::to_string(it.checkbox) + ":" + InlineSig(it.content) + "}";
    s += "|p" + std::to_string(b.paragraph_starts.size()) + (b.box_closed ? "c" : "");
    for (const std::string &k : b.field_order) {
        auto f = b.fields.find(k);
        s += k + "=" + (f == b.fields.end() ? "" : Norm(f->second)) + ";";
    }
    if (b.kind == BlockKind::Comment || b.kind == BlockKind::Callout) s += "|W{" + CommentWords(b.text) + "}";
    return s;
}

// --- refilling ------------------------------------------------------------

// A word of prose, or an inline `// comment`, which ends its line.
struct Tok {
    std::string text;
    bool comment = false;
};

// Marks the bytes of `s` no line break may fall inside: maths, verbatim,
// links, citations, raw text and command calls whole, and every other
// construct's markup and arguments (`\color(red, `, a closing `)`), so only
// the prose inside can break. False when an inline comment sits inside some
// construct (it would swallow whatever a refill put after it on its line).
bool MarkKeep(const std::vector<Inline> &ins, int depth, std::vector<char> *keep,
              std::vector<std::pair<int, int>> *comments) {
    const int n = static_cast<int>(keep->size());
    auto mark = [&](int a, int z) {
        for (int k = std::max(0, a); k < std::min(n, z); ++k) (*keep)[static_cast<size_t>(k)] = 1;
    };
    for (const Inline &x : ins) {
        switch (x.kind) {
            case InlineKind::Comment:
                if (depth > 0) return false;
                comments->emplace_back(x.start, x.end);
                break;
            case InlineKind::Verbatim:
            case InlineKind::Link:
            case InlineKind::Cite:
            case InlineKind::CiteP:
            case InlineKind::Math:
            case InlineKind::Raw:
            case InlineKind::Command:
                mark(x.start, x.end);
                break;
            case InlineKind::Text:
                if (x.inner_start > x.start) mark(x.start, x.inner_start + 1);  // an escape
                break;
            default:
                mark(x.start, x.inner_start);
                mark(x.inner_end, x.end);
                if (!MarkKeep(x.children, depth + 1, keep, comments)) return false;
                break;
        }
    }
    return true;
}

// The words of `s` (prose already parsed into `ins`, offsets into `s`).
std::optional<std::vector<Tok>> Tokens(const std::string &s, const std::vector<Inline> &ins) {
    std::vector<char> keep(s.size(), 0);
    std::vector<std::pair<int, int>> comments;
    if (!MarkKeep(ins, 0, &keep, &comments)) return std::nullopt;
    std::vector<Tok> toks;
    const size_t n = s.size();
    size_t i = 0, c = 0;
    while (i < n) {
        if (IsBlank(s[i]) && !keep[i]) {
            ++i;
            continue;
        }
        if (c < comments.size() && static_cast<int>(i) >= comments[c].first) {
            const size_t e = static_cast<size_t>(std::max(comments[c].second, static_cast<int>(i)));
            toks.push_back({RTrim(s.substr(i, e - i)), true});
            i = e;
            ++c;
            continue;
        }
        std::string w;
        while (i < n && !(IsBlank(s[i]) && !keep[i])) {
            if (c < comments.size() && static_cast<int>(i) == comments[c].first) break;
            if (IsBlank(s[i])) {
                // Whitespace inside a construct that can't break: a line
                // break in it (it was written over two lines) is a space.
                size_t j = i;
                bool nl = false;
                for (; j < n && IsBlank(s[j]) && keep[j]; ++j)
                    if (s[j] == '\n') nl = true;
                if (nl) {
                    w += ' ';
                    i = j;
                    continue;
                }
            }
            w += s[i++];
        }
        if (!w.empty()) toks.push_back({w, false});
    }
    return toks;
}

// Whether a line may start with `w`: it has to still read as a line of
// prose, not open a heading, list, table, comment, block or close a slide.
class LineStarts {
  public:
    bool Ok(const std::string &w) {
        auto it = cache_.find(w);
        if (it != cache_.end()) return it->second;
        bool ok = !w.empty() && w[0] != '|' && w[0] != '/' && w[0] != ')' && w[0] != '}';
        if (ok) ok = Prose(w) && Prose(w + " x") && Prose(w + " x)");
        cache_[w] = ok;
        return ok;
    }

  private:
    static bool Prose(const std::string &line) {
        const Document d = Parse({line});
        return d.blocks.size() == 1 && d.blocks[0].kind == BlockKind::Paragraph;
    }
    std::map<std::string, bool> cache_;
};

// What may not start a comment's line: it would turn into a callout, a
// header line or a results marker.
bool CommentLineStartOk(const std::string &w) {
    if (w.empty() || w[0] == '?' || w.rfind("result_", 0) == 0) return false;
    std::string kw = w;
    if (!kw.empty() && kw.back() == ':') kw.pop_back();
    for (const std::string &k : CalloutKeywords())
        if (k == kw) return false;
    return true;
}

// Greedy fill: words go on the line until the next would pass `width`. A
// word that may not start a line takes the word before it down with it,
// or, when that can't start one either, stays on the long line.
template <typename OkFn>
Lines Fill(const std::vector<Tok> &toks, const std::string &first, const std::string &rest, int width, OkFn ok) {
    Lines out;
    std::vector<std::string> line;
    std::string lead = first;
    auto text_of = [&](const std::vector<std::string> &ws) {
        std::string s = lead;
        for (size_t k = 0; k < ws.size(); ++k) s += (k ? " " : "") + ws[k];
        return s;
    };
    auto finish = [&](const std::string &s) {
        out.push_back(RTrim(s));
        lead = rest;
        line.clear();
    };
    for (const Tok &t : toks) {
        if (t.comment) {
            std::string s = text_of(line);
            finish(line.empty() ? s + t.text : s + " " + t.text);
            continue;
        }
        if (line.empty()) {
            line.push_back(t.text);
            continue;
        }
        std::vector<std::string> with = line;
        with.push_back(t.text);
        if (Cols(text_of(with)) <= width) {
            line = std::move(with);
            continue;
        }
        if (ok(t.text)) {
            finish(text_of(line));
            line.push_back(t.text);
        } else if (line.size() >= 2 && ok(line.back())) {
            const std::string carried = line.back();
            line.pop_back();
            finish(text_of(line));
            line = {carried, t.text};
        } else {
            line = std::move(with);
        }
    }
    if (!line.empty()) finish(text_of(line));
    if (out.empty()) out.push_back(RTrim(first));
    return out;
}

struct Formatter {
    const Lines &src;
    int width;
    LineStarts starts;

    // Prose (one paragraph, or one list item's text) refilled.
    std::optional<Lines> FillProse(const std::string &text, const std::string &first, const std::string &rest) {
        const std::optional<std::vector<Tok>> toks = Tokens(text, ParseInlines(text));
        if (!toks) return std::nullopt;
        return Fill(*toks, first, rest, width, [this](const std::string &w) { return starts.Ok(w); });
    }

    std::optional<Lines> Paragraph(const Block &b) {
        const std::string first = LeadingSpace(src[static_cast<size_t>(b.line_start)]);
        return FillProse(Trim(b.text), first, first);
    }

    std::optional<Lines> List(const Block &b) {
        Lines out;
        for (size_t k = 0; k < b.items.size(); ++k) {
            const ListItem &it = b.items[k];
            const int rel = it.line - b.line_start;
            if (rel < 0 || static_cast<size_t>(rel) >= b.line_offsets.size()) return std::nullopt;
            const int line_off = b.line_offsets[static_cast<size_t>(rel)];
            const std::string &marker_line = src[static_cast<size_t>(it.line)];
            const int col = it.content_start - line_off;
            if (col < 0 || static_cast<size_t>(col) > marker_line.size()) return std::nullopt;
            std::string prefix = marker_line.substr(0, static_cast<size_t>(col));
            if (!prefix.empty() && prefix.back() != ' ') prefix += ' ';
            const std::string content =
                it.content_end > it.content_start
                    ? b.text.substr(static_cast<size_t>(it.content_start),
                                    static_cast<size_t>(it.content_end - it.content_start))
                    : std::string();
            std::optional<Lines> filled = FillProse(content, prefix, Blanked(prefix));
            if (!filled) return std::nullopt;
            out.insert(out.end(), filled->begin(), filled->end());
        }
        // Lines before the first item (none: a list starts with one) or a
        // count that came out wrong would lose text; leave such a list be.
        if (b.items.empty() || b.items.front().line != b.line_start) return std::nullopt;
        return out;
    }

    bool Overflows(const Block &b) const {
        for (int i = b.line_start; i <= b.line_end; ++i)
            if (Cols(RTrim(src[static_cast<size_t>(i)])) > width) return true;
        return false;
    }

    // A comment run refilled, paragraph by paragraph: an empty `//` line
    // separates them, and a line indented past the `// ` or starting like
    // a list item keeps its own start (ascii art, code, a list in a
    // comment).
    std::optional<Lines> Comment(const Block &b) {
        if (!Overflows(b)) return std::nullopt;
        Lines out;
        std::vector<Tok> para;
        std::string lead;
        auto flush = [&] {
            if (para.empty()) return;
            Lines f = Fill(para, lead, lead, width, CommentLineStartOk);
            out.insert(out.end(), f.begin(), f.end());
            para.clear();
        };
        for (int i = b.line_start; i <= b.line_end; ++i) {
            const std::string &l = src[static_cast<size_t>(i)];
            const size_t p = l.find("//");
            if (p == std::string::npos) return std::nullopt;
            std::string body = l.substr(p + 2);
            if (!body.empty() && body[0] == ' ') body.erase(0, 1);
            const std::string here = l.substr(0, p) + "// ";
            const bool listy = !body.empty() && (body.rfind("- ", 0) == 0 || body.rfind("* ", 0) == 0 ||
                                                 (std::isdigit(static_cast<unsigned char>(body[0])) &&
                                                  body.find_first_not_of("0123456789") != std::string::npos &&
                                                  (body[body.find_first_not_of("0123456789")] == '.' ||
                                                   body[body.find_first_not_of("0123456789")] == ')')));
            if (Trim(body).empty()) {
                flush();
                out.push_back(RTrim(l.substr(0, p) + "//"));
                continue;
            }
            if (body[0] == ' ' || body[0] == '\t') {
                flush();
                out.push_back(RTrim(l));
                continue;
            }
            if (listy || para.empty()) {
                flush();
                lead = here;
            }
            size_t a = 0;
            while (a < body.size()) {
                while (a < body.size() && IsBlank(body[a])) ++a;
                size_t z = a;
                while (z < body.size() && !IsBlank(body[z])) ++z;
                if (z > a) para.push_back({body.substr(a, z - a), false});
                a = z;
            }
        }
        flush();
        return out;
    }

    // `// KEYWORD: text` and the `//` lines after it, refilled as one
    // paragraph after the keyword.
    std::optional<Lines> Callout(const Block &b) {
        if (!Overflows(b)) return std::nullopt;
        const std::string &l0 = src[static_cast<size_t>(b.line_start)];
        const size_t slashes = l0.find("//");
        if (slashes == std::string::npos) return std::nullopt;
        const size_t colon = l0.find(':', slashes + 2);
        if (colon == std::string::npos) return std::nullopt;
        std::string body = b.text;  // leaders blanked, as the parser reads it
        for (int i = b.line_start + 1; i <= b.line_end; ++i) {
            const std::string &l = src[static_cast<size_t>(i)];
            const size_t p = l.find("//");
            if (p == std::string::npos) return std::nullopt;
            const size_t off = static_cast<size_t>(b.line_offsets[static_cast<size_t>(i - b.line_start)]);
            for (size_t q = 0; q < p + 2; ++q) body[off + q] = ' ';
        }
        const std::string text = body.substr(colon + 1);
        const std::optional<std::vector<Tok>> toks = Tokens(text, ParseInlines(text));
        if (!toks) return std::nullopt;
        for (const Tok &t : *toks)
            if (t.comment) return std::nullopt;
        return Fill(*toks, l0.substr(0, colon + 1) + " ", l0.substr(0, slashes) + "// ", width,
                    [](const std::string &w) { return CommentLineStartOk(w); });
    }

    // Columns padded to line the pipes up. Only a table whose rows all have
    // both outer pipes: rows without them keep the way they were written.
    std::optional<Lines> Table(const Block &b) {
        const int last = b.rows_end >= b.line_start ? b.rows_end : b.line_end;
        std::vector<std::vector<std::string>> cells;
        std::vector<bool> sep;
        for (int i = b.line_start; i <= last; ++i) {
            const std::string &l = src[static_cast<size_t>(i)];
            const std::string t = Trim(l);
            if (t.size() < 2 || t.front() != '|' || t.back() != '|' || t[t.size() - 2] == '\\') return std::nullopt;
            std::vector<std::string> row;
            bool is_sep = true;
            for (const auto &c : TableCells(l)) {
                row.push_back(Trim(l.substr(static_cast<size_t>(c.first), static_cast<size_t>(c.second - c.first))));
                if (row.back().empty() || row.back().find_first_not_of("-:=") != std::string::npos) is_sep = false;
            }
            if (row.empty()) return std::nullopt;
            cells.push_back(std::move(row));
            sep.push_back(is_sep && i > b.line_start);
        }
        size_t ncol = 0;
        for (const auto &r : cells) ncol = std::max(ncol, r.size());
        std::vector<int> w(ncol, 3);
        for (size_t r = 0; r < cells.size(); ++r)
            if (!sep[r])
                for (size_t c = 0; c < cells[r].size(); ++c) w[c] = std::max(w[c], Cols(cells[r][c]));
        const std::string indent = LeadingSpace(src[static_cast<size_t>(b.line_start)]);
        Lines out;
        for (size_t r = 0; r < cells.size(); ++r) {
            std::string line = indent + "|";
            for (size_t c = 0; c < cells[r].size(); ++c) {
                const std::string &v = cells[r][c];
                const int pad = w[c] - Cols(v);
                std::string cell;
                if (sep[r]) {
                    const bool left = v.front() == ':', right = v.size() > 1 && v.back() == ':';
                    const char ch = v.find('=') != std::string::npos ? '=' : '-';
                    cell = std::string(left ? ":" : "") +
                           std::string(static_cast<size_t>(std::max(1, w[c] - (left ? 1 : 0) - (right ? 1 : 0))), ch) +
                           (right ? ":" : "");
                } else {
                    const Align a = c < b.aligns.size() ? b.aligns[c] : Align::Default;
                    const int before = a == Align::Right ? pad : a == Align::Center ? pad / 2 : 0;
                    cell = std::string(static_cast<size_t>(before), ' ') + v +
                           std::string(static_cast<size_t>(pad - before), ' ');
                }
                line += " " + cell + " |";
            }
            out.push_back(line);
        }
        for (int i = last + 1; i <= b.line_end; ++i) out.push_back(RTrim(src[static_cast<size_t>(i)]));
        return out;
    }

    // `\abstract(` ... `)`: each paragraph inside refilled.
    std::optional<Lines> Abstract(const Block &b) {
        if (Trim(src[static_cast<size_t>(b.line_start)]) != "\\abstract(" ||
            Trim(src[static_cast<size_t>(b.line_end)]) != ")" || b.line_end - b.line_start < 2)
            return std::nullopt;
        Lines out = {RTrim(src[static_cast<size_t>(b.line_start)])};
        int i = b.line_start + 1;
        while (i < b.line_end) {
            if (Trim(src[static_cast<size_t>(i)]).empty()) {
                if (!out.empty() && !out.back().empty() && out.size() > 1) out.push_back("");
                ++i;
                continue;
            }
            int j = i;
            while (j + 1 < b.line_end && !Trim(src[static_cast<size_t>(j + 1)]).empty()) ++j;
            const std::string first = LeadingSpace(src[static_cast<size_t>(i)]);
            std::optional<Lines> f = FillProse(Trim(Join(src, static_cast<size_t>(i), static_cast<size_t>(j))), first, first);
            if (!f) return std::nullopt;
            out.insert(out.end(), f->begin(), f->end());
            i = j + 1;
        }
        if (out.back().empty()) out.pop_back();
        out.push_back(RTrim(src[static_cast<size_t>(b.line_end)]));
        return out;
    }
};

struct Edit {
    int first = 0, last = 0;  // lines of the original, inclusive
    Lines lines;
};

Lines Apply(const Lines &src, const std::vector<Edit> &edits, const std::vector<bool> &use) {
    Lines out;
    int at = 0;
    for (size_t k = 0; k < edits.size(); ++k) {
        if (!use[k]) continue;
        const Edit &e = edits[k];
        for (; at < e.first; ++at) out.push_back(src[static_cast<size_t>(at)]);
        out.insert(out.end(), e.lines.begin(), e.lines.end());
        at = e.last + 1;
    }
    for (; at < static_cast<int>(src.size()); ++at) out.push_back(src[static_cast<size_t>(at)]);
    return out;
}

}  // namespace

std::string DocumentSignature(const Document &doc) {
    std::string s = "title=" + Norm(doc.title) + "\n";
    for (const auto &m : doc.meta) s += "meta " + m.first + "=" + Norm(m.second) + "\n";
    for (const Option &o : doc.options) s += "opt " + o.name + "=" + o.value.s + "\n";
    for (const auto &c : doc.commands) {
        s += "cmd " + c.first + "(";
        for (const std::string &p : c.second.params) s += p + ",";
        s += ")=" + Norm(c.second.body) + "\n";
    }
    for (const std::string &k : doc.cite_order) s += "cite " + k + "\n";
    s += "footnotes=" + std::to_string(doc.footnote_count) + "\n";
    for (const Block &b : doc.blocks) s += BlockSig(b) + "\n";
    for (const Diagnostic &d : doc.diagnostics) {
        // Messages can name a line number, which formatting may move.
        std::string m;
        for (char c : d.message)
            if (!std::isdigit(static_cast<unsigned char>(c))) m += c;
        s += "diag " + std::to_string(static_cast<int>(d.severity)) + " " + m + "\n";
    }
    return s;
}

FormatDocumentResult FormatDocument(const std::string &text, const FormatDocumentOptions &opt) {
    FormatDocumentResult res;
    const Lines src = SplitLines(text);
    const int n = static_cast<int>(src.size());
    const Document doc = Parse(src);
    const std::string want = DocumentSignature(doc);

    // Lines formatting leaves exactly as they are: code blocks whole (with
    // their results -- a markdown region's blocks are a program's output),
    // and the blocks whose text is verbatim.
    std::vector<bool> frozen(static_cast<size_t>(n), false);
    std::vector<bool> in_block(static_cast<size_t>(n), false);
    auto freeze = [&](int a, int z) {
        for (int i = std::max(0, a); i <= std::min(n - 1, z); ++i) frozen[static_cast<size_t>(i)] = true;
    };
    for (const Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        for (int i = std::max(0, b.line_start); i <= std::min(n - 1, b.line_end); ++i) in_block[static_cast<size_t>(i)] = true;
        switch (b.kind) {
            case BlockKind::Code: freeze(b.line_start, std::max(b.line_end, b.result_line_end)); break;
            case BlockKind::Define:
            case BlockKind::Raw:
            case BlockKind::Command:
            case BlockKind::Html:
            case BlockKind::Svg:
            case BlockKind::Citation: freeze(b.line_start, b.line_end); break;
            default: break;
        }
    }

    Formatter f{src, std::max(20, opt.width), {}};
    std::vector<Edit> edits;
    auto add = [&](int first, int last, Lines lines) {
        Lines was(src.begin() + first, src.begin() + last + 1);
        if (was != lines) edits.push_back({first, last, std::move(lines)});
    };
    for (const Block &b : doc.blocks) {
        if (!b.origin.empty() || b.line_start < 0 || b.line_end >= n || b.line_end < b.line_start) continue;
        if (frozen[static_cast<size_t>(b.line_start)]) continue;
        std::optional<Lines> out;
        switch (b.kind) {
            case BlockKind::Paragraph: out = f.Paragraph(b); break;
            case BlockKind::List: out = f.List(b); break;
            case BlockKind::Comment: out = f.Comment(b); break;
            case BlockKind::Callout: out = f.Callout(b); break;
            case BlockKind::Table: out = f.Table(b); break;
            case BlockKind::Abstract: out = f.Abstract(b); break;
            default: break;
        }
        if (!out) {
            out = Lines();
            for (int i = b.line_start; i <= b.line_end; ++i) out->push_back(RTrim(src[static_cast<size_t>(i)]));
        }
        add(b.line_start, b.line_end, std::move(*out));
    }
    // Blank lines between blocks: one where there were several, none at
    // the top or the bottom of the file.
    for (int i = 0; i < n;) {
        if (in_block[static_cast<size_t>(i)] || frozen[static_cast<size_t>(i)] || !Trim(src[static_cast<size_t>(i)]).empty()) {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < n && !in_block[static_cast<size_t>(j + 1)] && !frozen[static_cast<size_t>(j + 1)] &&
               Trim(src[static_cast<size_t>(j + 1)]).empty())
            ++j;
        add(i, j, i == 0 || j == n - 1 ? Lines() : Lines{""});
        i = j + 1;
    }
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.first < b.first; });

    // The self-check. Normally everything passes at once; when it doesn't,
    // each change is tried on its own and the ones that alter the document
    // are dropped -- then the rest together, adding them one at a time
    // should two that pass alone still clash.
    std::vector<bool> use(edits.size(), true);
    auto same = [&](const std::vector<bool> &u) { return DocumentSignature(Parse(Apply(src, edits, u))) == want; };
    if (!same(use)) {
        std::vector<bool> one(edits.size(), false);
        for (size_t k = 0; k < edits.size(); ++k) {
            one[k] = true;
            use[k] = same(one);
            one[k] = false;
        }
        if (!same(use)) {
            std::vector<bool> acc(edits.size(), false);
            for (size_t k = 0; k < edits.size(); ++k) {
                if (!use[k]) continue;
                acc[k] = true;
                if (!same(acc)) acc[k] = false;
            }
            use = acc;
        }
        for (bool u : use)
            if (!u) ++res.kept;
    }
    const Lines out = Apply(src, edits, use);
    for (const std::string &l : out) res.text += l + "\n";
    return res;
}

}  // namespace mepml
