// The analysis half of mep's mepml language server -- see mepml_lsp.h.
// Every answer is read off mepml::Parse / ParseWithImports (mepml_doc.h),
// never a second reading of the syntax.

#include "mepml_lsp.h"

#include <fstream>
#include <sstream>

#include "mepml_style.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <system_error>

#include "mepml_doc.h"
#include "org_lsp.h"

using mepml::Block;
using mepml::BlockKind;
using mepml::Document;
using mepml::Inline;
using mepml::InlineKind;

namespace {

// --- Small helpers ----------------------------------------------------------

bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

bool StartsWithCI(const std::string &s, const std::string &prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

int Codepoints(const std::string &s) {
    int n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

int Len(const std::string &s) { return static_cast<int>(s.size()); }

// Edit distance, for "did you mean" fixes; small inputs only.
int Distance(const std::string &a, const std::string &b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// The closest candidate within a distance that still reads as a typo.
std::string Nearest(const std::string &word, const std::vector<std::string> &candidates) {
    std::string best;
    int best_d = std::max(1, std::min(3, Len(word) / 3 + 1)) + 1;
    for (const std::string &c : candidates) {
        const int d = Distance(Lower(word), Lower(c));
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

bool ReadFileLines(const std::string &path, std::vector<std::string> *lines) {
    std::ifstream f(path);
    if (!f) return false;
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines->push_back(l);
    }
    return true;
}

bool FileExists(const std::string &path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string DocDir(const MepmlLspOptions &opts) {
    if (opts.doc_path.empty()) return "";
    return std::filesystem::path(opts.doc_path).parent_path().string();
}

// A path as a document names it, resolved against the document's directory.
std::string Resolve(const MepmlLspOptions &opts, const std::string &path) {
    if (path.empty() || path[0] == '/') return path;
    if (!path.empty() && path[0] == '~') {
        const char *home = std::getenv("HOME");
        return home ? std::string(home) + path.substr(1) : path;
    }
    std::string p = path;
    if (p.rfind("./", 0) == 0) p = p.substr(2);
    const std::string dir = DocDir(opts);
    return dir.empty() ? p : dir + "/" + p;
}

// What a link target is: a heading anchor, another file, or somewhere else.
bool IsExternal(const std::string &url) {
    const size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 1) {
        bool scheme = true;
        for (size_t i = 0; i < colon; ++i)
            scheme = scheme && (std::isalnum(static_cast<unsigned char>(url[i])) != 0 || url[i] == '+' || url[i] == '-' || url[i] == '.');
        if (scheme) return url.rfind("file:", 0) != 0;
    }
    return url.rfind("www.", 0) == 0;
}
std::string FilePart(std::string url) {
    if (url.rfind("file:", 0) == 0) url = url.substr(5);
    const size_t hash = url.find('#');
    if (hash != std::string::npos) url = url.substr(0, hash);
    return url;
}

// --- The parsed document -------------------------------------------------------

// Every request used to parse the document again -- hover, completion,
// symbols and folds each cost a full parse (5.7 ms at 16k lines;
// plans/MEPML_PERFORMANCE_PLAN.md). The server answers one request at a
// time, so one cached parse per kind (plain, and with imports) keyed on
// the text itself does: an edit changes the text and so the key, and an
// imported file is re-read when its modification time moves.
long long LspFileMtime(const std::string &path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
}

struct CachedParse {
    bool valid = false;
    std::vector<std::string> lines;
    std::string doc_path;
    std::vector<std::pair<std::string, long long>> reads;
    Document doc;
};

const Document &ParsePlain(const std::vector<std::string> &lines) {
    static CachedParse c;
    if (!(c.valid && c.lines == lines)) {
        c.doc = mepml::Parse(lines);
        c.lines = lines;
        c.valid = true;
    }
    return c.doc;
}

const Document &Analyze(const std::vector<std::string> &lines, const MepmlLspOptions &opts) {
    if (!(opts.check_files && !opts.doc_path.empty())) return ParsePlain(lines);
    static CachedParse c;
    bool fresh = c.valid && c.doc_path == opts.doc_path && c.lines == lines;
    for (const auto &r : c.reads) {
        if (!fresh) break;
        fresh = LspFileMtime(r.first) == r.second;
    }
    if (!fresh) {
        c.reads.clear();
        c.doc = mepml::ParseWithImports(opts.doc_path, lines, [](const std::string &p, std::vector<std::string> *l) {
            c.reads.emplace_back(p, LspFileMtime(p));
            return ReadFileLines(p, l);
        });
        c.lines = lines;
        c.doc_path = opts.doc_path;
        c.valid = true;
    }
    return c.doc;
}

struct Pos {
    int line = 0, col = 0;
};
Pos At(const Block &b, int offset) {
    const Block::Pos p = b.OffsetToPos(offset);
    return {p.line, p.col};
}

// Every inline of a block (paragraph, heading, callout, caption, list
// items, table cells), outermost first.
void ForEachInline(const Block &b, const std::function<void(const Inline &)> &fn) {
    std::function<void(const std::vector<Inline> &)> walk = [&](const std::vector<Inline> &ins) {
        for (const Inline &x : ins) {
            fn(x);
            walk(x.children);
        }
    };
    walk(b.inlines);
    walk(b.caption_inlines);
    for (const mepml::ListItem &it : b.items) walk(it.content);
    for (const auto &row : b.rows)
        for (const mepml::TableCell &c : row) walk(c.content);
}

// This document's own blocks (not those an \import brought in).
template <typename Fn>
void ForEachOwnBlock(const Document &doc, Fn fn) {
    for (const Block &b : doc.blocks)
        if (b.origin.empty()) fn(b);
}

const Block *OwnBlockAt(const Document &doc, int line) {
    for (const Block &b : doc.blocks)
        if (b.origin.empty() && b.line_start <= line && line <= b.line_end) return &b;
    return nullptr;
}

// The innermost inline under (line, col) in block b, or nullptr.
const Inline *InlineAt(const Block &b, int line, int col) {
    const int rel = line - b.line_start;
    if (rel < 0 || rel >= static_cast<int>(b.line_offsets.size())) return nullptr;
    const int off = b.line_offsets[static_cast<size_t>(rel)] + col;
    const Inline *best = nullptr;
    ForEachInline(b, [&](const Inline &x) {
        if (x.start <= off && off < x.end && (best == nullptr || (x.end - x.start) <= (best->end - best->start))) best = &x;
    });
    return best;
}

// A heading, as the outline and anchors see it.
struct Heading {
    int line = 0;
    int level = 0;
    std::string title;  // plain text
    std::string slug;
    int title_col = 0;  // where the title's text starts on its line
    int title_end = 0;
};
std::vector<Heading> Headings(const Document &doc, const std::vector<std::string> &lines) {
    std::vector<Heading> hs;
    ForEachOwnBlock(doc, [&](const Block &b) {
        if (b.kind != BlockKind::Heading || b.line_start < 0 || b.line_start >= static_cast<int>(lines.size())) return;
        Heading h;
        h.line = b.line_start;
        h.level = b.level;
        h.title = mepml::InlinePlainText(b.inlines);
        h.slug = mepml::HeadingSlug(h.title);
        const std::string &l = lines[static_cast<size_t>(h.line)];
        h.title_col = mepml::LineHeadingMarkupLen(l);
        size_t end = l.find_last_not_of(" \t");
        h.title_end = end == std::string::npos ? Len(l) : static_cast<int>(end) + 1;
        hs.push_back(h);
    });
    return hs;
}
// The heading a `#anchor` names: its slug, or (as mep's link following
// allows) its title as written.
const Heading *HeadingForAnchor(const std::vector<Heading> &hs, const std::string &anchor) {
    for (const Heading &h : hs)
        if (h.slug == Lower(anchor) || h.title == anchor || h.slug == mepml::HeadingSlug(anchor)) return &h;
    return nullptr;
}

// Where a \citation / bibtex entry's key sits on its first line.
struct CitationDef {
    std::string key;
    std::string origin;  // "" for this document
    int line = 0;
    int col_start = 0, col_end = 0;
};
std::vector<CitationDef> CitationDefs(const Document &doc, const std::vector<std::string> &lines) {
    std::vector<CitationDef> out;
    for (const Block &b : doc.blocks) {
        if (b.kind != BlockKind::Citation || b.value.empty()) continue;
        CitationDef d;
        d.key = b.value;
        d.origin = b.origin;
        d.line = b.line_start;
        if (b.origin.empty() && b.line_start >= 0 && b.line_start < static_cast<int>(lines.size())) {
            const std::string &l = lines[static_cast<size_t>(b.line_start)];
            const size_t brace = l.find_first_of("{(");
            if (brace != std::string::npos) {
                const size_t k = l.find(b.value, brace + 1);
                if (k != std::string::npos) {
                    d.col_start = static_cast<int>(k);
                    d.col_end = static_cast<int>(k + b.value.size());
                }
            }
        }
        out.push_back(d);
    }
    return out;
}

// A \cite's key range on its line (the braces' content, trimmed).
struct KeyRange {
    int line = 0, col_start = 0, col_end = 0;
};
KeyRange CiteKeyRange(const Block &b, const Inline &x) {
    int s = x.inner_start, e = x.inner_end;
    while (s < e && std::isspace(static_cast<unsigned char>(b.text[static_cast<size_t>(s)]))) ++s;
    while (e > s && std::isspace(static_cast<unsigned char>(b.text[static_cast<size_t>(e - 1)]))) --e;
    const Pos p = At(b, s), q = At(b, e);
    return {p.line, p.col, q.line == p.line ? q.col : p.col};
}

// A link's url range on its line.
KeyRange LinkUrlRange(const Block &b, const Inline &x) {
    const bool has_bar = x.inner_end < x.end - 1 && b.text[static_cast<size_t>(x.inner_end)] == '|';
    int s = has_bar ? x.inner_end + 1 : x.inner_start;
    int e = has_bar ? x.end - 1 : x.inner_end;
    while (s < e && b.text[static_cast<size_t>(s)] == ' ') ++s;
    while (e > s && b.text[static_cast<size_t>(e - 1)] == ' ') --e;
    const Pos p = At(b, s), q = At(b, e);
    return {p.line, p.col, q.line == p.line ? q.col : p.col};
}

// \color(name, ...)'s (or \color{name}{...}'s) name range.
KeyRange CommandArgRange(const Block &b, const Inline &x) {
    const size_t open = b.text.find_first_of("{(", static_cast<size_t>(x.start));
    const size_t close = open == std::string::npos ? open : b.text.find(b.text[open] == '(' ? ',' : '}', open);
    if (close == std::string::npos) {
        const Pos p = At(b, x.start);
        return {p.line, p.col, p.col};
    }
    const Pos p = At(b, static_cast<int>(open) + 1), q = At(b, static_cast<int>(close));
    return {p.line, p.col, q.line == p.line ? q.col : p.col};
}

// The path inside `\name(path)` (or `@name{path}`) on a directive line.
KeyRange DirectivePathRange(const std::string &line, int line_no) {
    const size_t open = line.find_first_of("{(");
    size_t close = std::string::npos;
    if (open != std::string::npos) {
        const char o = line[open], c = o == '(' ? ')' : '}';
        int depth = 0;
        for (size_t k = open; k < line.size() && close == std::string::npos; ++k) {
            if (line[k] == '\\') ++k;
            else if (line[k] == o) ++depth;
            else if (line[k] == c && --depth == 0) close = k;
        }
    }
    if (close == std::string::npos) return {line_no, 0, 0};
    return {line_no, static_cast<int>(open) + 1, static_cast<int>(close)};
}

std::string CitationSummary(const Document &doc, const std::string &key) {
    auto it = doc.citations.find(key);
    if (it == doc.citations.end()) return "";
    const mepml::BibEntryParts e = mepml::BibEntry(it->second);
    return e.lead + e.title + e.rest;
}

// --- Diagnostic codes for the parser's own messages ----------------------------

struct CodeRule {
    const char *prefix;
    const char *code;
};
std::string CodeFor(const std::string &message) {
    static const CodeRule kRules[] = {
        {"unknown citation key", "unknown-citation"},
        {"unknown directive", "unknown-directive"},
        {"@printbibliography is now", "deprecated-directive"},
        {"@abstract{...} is now", "deprecated-directive"},
        {"unexpected text after directive", "trailing-text"},
        {"Option: expects", "bad-option"},
        {"code block is never closed", "unclosed-code"},
        {"results region has no", "unclosed-results"},
        {"results marker not attached", "orphan-results"},
        {"display math is never closed", "unclosed-math"},
        {"\\slide is never closed", "unclosed-slide"},
        {"unknown document type", "unknown-type"},
        {"a presentation with no", "empty-presentation"},
        {"unknown export format", "unknown-export"},
        {"a Beamer export with no", "empty-presentation"},
        {"not on any slide", "off-slide"},
        {"a slide's content goes on", "trailing-text"},
        {"citation has no key", "citation-no-key"},
        {"duplicate citation key", "duplicate-citation"},
        {"circular import", "import-cycle"},
        {"cannot read import", "import-missing"},
        {"already imported", "duplicate-import"},
    };
    for (const CodeRule &r : kRules)
        if (message.rfind(r.prefix, 0) == 0) return r.code;
    // A failure inside an imported file, reported on the line importing it.
    if (message.find("circular import") != std::string::npos) return "import-cycle";
    if (message.find("cannot read import") != std::string::npos) return "import-missing";
    if (message.find("must directly follow") != std::string::npos) return "orphan-caption";
    if (message.find("needs a path") != std::string::npos) return "missing-path";
    if (message.rfind("unterminated \\", 0) == 0 || message.rfind("unterminated @", 0) == 0) return "unterminated-directive";
    return "syntax";
}

}  // namespace

// ===========================================================================
// Diagnostics
// ===========================================================================

std::vector<MepmlLspDiagnostic> MepmlLspDiagnostics(const std::vector<std::string> &lines, const MepmlLspOptions &opts) {
    const Document &doc = Analyze(lines, opts);
    std::vector<MepmlLspDiagnostic> out;
    auto add = [&](int line, int cs, int ce, MepmlLspSeverity sev, const std::string &code, const std::string &msg) {
        MepmlLspDiagnostic d;
        d.line = line;
        d.col_start = cs;
        d.col_end = std::max(ce, cs);
        d.severity = sev;
        d.code = code;
        d.message = msg;
        out.push_back(d);
    };

    for (const mepml::Diagnostic &d : doc.diagnostics) {
        const MepmlLspSeverity sev = d.severity == mepml::Diagnostic::Error     ? MepmlLspSeverity::Error
                                     : d.severity == mepml::Diagnostic::Warning ? MepmlLspSeverity::Warning
                                                                                : MepmlLspSeverity::Information;
        add(d.line, d.col_start, d.col_end, sev, CodeFor(d.message), d.message);
    }

    const std::vector<Heading> heads = Headings(doc, lines);
    const bool files = opts.check_files && !opts.doc_path.empty();

    // Accessibility: what a reader who cannot see the page is left without
    // (mepml_a11y.h has the whole check; these are the two an author fixes
    // on the spot). A figure with no \alttext says nothing to a screen
    // reader -- `\alttext()` marks one that only decorates -- and a table
    // with no header row reads as values of nothing.
    for (const Block &b : doc.blocks) {
        if (!b.origin.empty()) continue;
        auto line_len = [&](int line) { return line >= 0 && line < static_cast<int>(lines.size()) ? Len(lines[static_cast<size_t>(line)]) : 0; };
        if (b.kind == BlockKind::Image && b.alt_line < 0)
            add(b.line_start, 0, line_len(b.line_start), MepmlLspSeverity::Hint, "a11y-figure-alt",
                "figure has no \\alttext(...) for readers who cannot see it (\\alttext() if it only decorates)");
        if (b.kind == BlockKind::Code && !b.result_images.empty() && b.alt_line < 0)
            add(b.result_images.front().first, 0, line_len(b.result_images.front().first), MepmlLspSeverity::Hint, "a11y-figure-alt",
                "the figure this block draws has no \\alttext(...) for readers who cannot see it");
        if (b.kind == BlockKind::Table && b.header_rows == 0 && b.rows.size() > 1)
            add(b.line_start, 0, line_len(b.line_start), MepmlLspSeverity::Hint, "a11y-table-header",
                "table has no header row (a |---| line under its first row): a screen reader cannot say what its values are");
    }

    // Headings that skip a level (> then >>>): the outline has a hole.
    int prev_level = 0;
    for (const Heading &h : heads) {
        if (prev_level > 0 && h.level > prev_level + 1)
            add(h.line, 0, h.title_col, MepmlLspSeverity::Hint, "heading-skip",
                "heading level " + std::to_string(h.level) + " directly under level " + std::to_string(prev_level));
        prev_level = h.level;
    }

    // Citations this document defines and never cites.
    std::set<std::string> cited;
    ForEachOwnBlock(doc, [&](const Block &b) {
        ForEachInline(b, [&](const Inline &x) {
            if (x.kind == InlineKind::Cite || x.kind == InlineKind::CiteP) cited.insert(x.text);
        });
    });
    for (const CitationDef &c : CitationDefs(doc, lines))
        if (c.origin.empty() && !cited.count(c.key))
            add(c.line, c.col_start, c.col_end, MepmlLspSeverity::Hint, "unused-citation",
                "citation '" + c.key + "' is never cited");

    // Document options set twice -- in this file (an import's options
    // are inherited only where this file does not set its own).
    {
        std::map<std::string, int> seen;
        std::vector<mepml::Option> own;
        ForEachOwnBlock(doc, [&](const Block &b) {
            if (b.kind == BlockKind::Meta) own.insert(own.end(), b.options.begin(), b.options.end());
        });
        for (const mepml::Option &o : own) {
            if (o.line < 0 || o.line >= static_cast<int>(lines.size())) continue;
            if (seen.count(o.name))
                add(o.line, 0, Len(lines[static_cast<size_t>(o.line)]), MepmlLspSeverity::Warning, "duplicate-option",
                    "option '" + o.name + "' is already set on line " + std::to_string(seen[o.name] + 1));
            else
                seen[o.name] = o.line;
        }
    }

    ForEachOwnBlock(doc, [&](const Block &b) {
        // Inline cross-references.
        ForEachInline(b, [&](const Inline &x) {
            if (x.kind == InlineKind::Link) {
                const KeyRange r = LinkUrlRange(b, x);
                if (!x.arg.empty() && x.arg[0] == '#') {
                    if (!HeadingForAnchor(heads, x.arg.substr(1)))
                        add(r.line, r.col_start, r.col_end, MepmlLspSeverity::Warning, "unknown-anchor",
                            "no heading has the anchor " + x.arg);
                } else if (files && !IsExternal(x.arg) && !FilePart(x.arg).empty() && !FileExists(Resolve(opts, FilePart(x.arg)))) {
                    add(r.line, r.col_start, r.col_end, MepmlLspSeverity::Warning, "missing-file",
                        "link target " + FilePart(x.arg) + " does not exist");
                }
            } else if (x.kind == InlineKind::Color) {
                std::uint32_t rgb = 0;
                if (!mepml::ParseColor(x.arg, &rgb)) {
                    const KeyRange r = CommandArgRange(b, x);
                    add(r.line, r.col_start, r.col_end, MepmlLspSeverity::Warning, "unknown-color",
                        "unknown colour '" + x.arg + "' (a name such as red, or #rrggbb)");
                }
            } else if (x.kind == InlineKind::Footnote) {
                if (Trim(mepml::InlinePlainText(x.children)).empty()) {
                    const Pos p = At(b, x.start), q = At(b, x.end);
                    add(p.line, p.col, q.line == p.line ? q.col : p.col + 1, MepmlLspSeverity::Warning, "empty-footnote",
                        "empty footnote");
                }
            }
        });
        // A style sheet the header names: that it is there, and what in it
        // could not be understood (the sheet still applies without it).
        if (files && b.kind == BlockKind::Meta && Lower(b.keyword) == "style" && !Trim(b.value).empty()) {
            const std::string &text = lines[static_cast<size_t>(b.line_start)];
            const size_t colon = text.find(':');
            const size_t from = colon == std::string::npos ? std::string::npos : text.find_first_not_of(" \t", colon + 1);
            const int cs = from == std::string::npos ? 0 : static_cast<int>(from), ce = Len(text);
            const std::string path = Resolve(opts, Trim(b.value));
            if (!FileExists(path)) {
                add(b.line_start, cs, ce, MepmlLspSeverity::Warning, "style-missing", "style sheet " + Trim(b.value) + " does not exist");
            } else {
                std::ifstream f(path);
                std::stringstream ss;
                ss << f.rdbuf();
                const mepml::style::Sheet sheet = mepml::style::Parse(ss.str(), path);
                const size_t shown = std::min<size_t>(sheet.diagnostics.size(), 5);
                for (size_t i = 0; i < shown; ++i)
                    add(b.line_start, cs, ce, MepmlLspSeverity::Warning, "style-error",
                        Trim(b.value) + ":" + std::to_string(sheet.diagnostics[i].line + 1) + ": " + sheet.diagnostics[i].message);
                if (sheet.diagnostics.size() > shown)
                    add(b.line_start, cs, ce, MepmlLspSeverity::Warning, "style-error",
                        Trim(b.value) + ": and " + std::to_string(sheet.diagnostics.size() - shown) + " more");
            }
        }
        // A sheet written in the document (`\raw(style, ...)`): what in it
        // could not be understood, on its own lines.
        if (b.kind == BlockKind::Raw && Lower(Trim(b.lang)) == "style") {
            const mepml::style::Sheet sheet = mepml::style::Parse(b.code, "");
            // (The text starts after `\raw(style,`: on that line, or the next.)
            const std::string &first = lines[static_cast<size_t>(b.line_start)];
            const size_t comma = first.find(',');
            const bool own_line = comma == std::string::npos || first.find_first_not_of(" \t", comma + 1) == std::string::npos;
            for (size_t i = 0; i < sheet.diagnostics.size() && i < 8; ++i) {
                const int line = std::clamp(b.line_start + sheet.diagnostics[i].line + (own_line ? 1 : 0), b.line_start, b.line_end);
                add(line, 0, Len(lines[static_cast<size_t>(line)]), MepmlLspSeverity::Warning, "style-error", sheet.diagnostics[i].message);
            }
        }
        // Files a block names.
        if (files && b.kind == BlockKind::Image && !b.value.empty() && !FileExists(Resolve(opts, b.value))) {
            const KeyRange r = DirectivePathRange(lines[static_cast<size_t>(b.line_start)], b.line_start);
            add(r.line, r.col_start, r.col_end, MepmlLspSeverity::Warning, "image-missing", "image " + b.value + " does not exist");
        }
        if (files && b.kind == BlockKind::Code) {
            for (const auto &img : b.result_images) {
                if (FileExists(Resolve(opts, img.second))) continue;
                const KeyRange r = DirectivePathRange(lines[static_cast<size_t>(img.first)], img.first);
                add(r.line, r.col_start, r.col_end, MepmlLspSeverity::Warning, "image-missing",
                    "figure " + img.second + " does not exist (run the block again to redraw it)");
            }
        }
        // Tables whose rows disagree about the column count.
        if (b.kind == BlockKind::Table && !b.rows.empty()) {
            size_t cols = 0;
            for (const auto &row : b.rows) cols = std::max(cols, row.size());
            for (const auto &row : b.rows) {
                if (row.size() == cols || row.empty()) continue;
                const Pos p = At(b, row.front().start);
                add(p.line, 0, Len(lines[static_cast<size_t>(p.line)]), MepmlLspSeverity::Warning, "table-columns",
                    "row has " + std::to_string(row.size()) + " cells; the table has " + std::to_string(cols));
            }
        }
    });

    std::sort(out.begin(), out.end(), [](const MepmlLspDiagnostic &a, const MepmlLspDiagnostic &b) {
        return a.line != b.line ? a.line < b.line : a.col_start < b.col_start;
    });
    return out;
}

// ===========================================================================
// Completion
// ===========================================================================

namespace {

struct Candidate {
    std::string text;   // what the typed segment completes to
    std::string label;  // shown ("" = text)
    MepmlLspKind kind = MepmlLspKind::Keyword;
    std::string detail;
    std::string doc;
    std::string suffix;  // appended after the completed text ("{", ": ", "=")
};

// Offers every candidate the typed segment is a prefix of. Only the word
// characters at the end of `typed` are replaced (see mepml_lsp.h), so the
// insertion is the candidate's remainder from where that word starts.
void Offer(std::vector<MepmlLspCompletionItem> &out, const std::string &typed, int col, const std::vector<Candidate> &cands) {
    int word = 0;
    while (word < Len(typed) && IsWordChar(typed[typed.size() - 1 - static_cast<size_t>(word)])) ++word;
    const size_t keep = typed.size() - static_cast<size_t>(word);
    std::set<std::string> seen;
    for (const Candidate &c : cands) {
        if (!StartsWithCI(c.text, typed) || c.text.size() < keep || !seen.insert(c.text).second) continue;
        MepmlLspCompletionItem it;
        it.label = c.label.empty() ? c.text : c.label;
        it.insert_text = c.text.substr(keep) + c.suffix;
        it.kind = c.kind;
        it.detail = c.detail;
        it.documentation = c.doc;
        it.replace_start = col - word;
        it.replace_end = col;
        out.push_back(it);
    }
}

struct Vocab {
    const char *name;
    const char *detail;
    const char *doc;
};

const std::vector<Vocab> &DirectiveVocab() {
    static const std::vector<Vocab> v = {
        {"image", "\\image(path)", "A figure: the picture at path (relative to this file). Follow it with \\caption() and \\alttext()."},
        {"caption", "\\caption(text)", "The caption of the image, table, code block or display maths right above. Numbered as Figure N / Table N."},
        {"alttext", "\\alttext(text)", "What stands for the figure, maths or table above for readers who cannot see it: a screen reader reads it in a figure's place, as how a formula is said, as what a table shows. An empty one -- \\alttext() -- under a figure says it only decorates."},
        {"import", "\\import(path)", "Includes another mepml (or .bib) file here: its blocks, citations and options join this document."},
        {"citation", "\\citation(key, fields)", "A bibliography entry: \\citation(key, author = ..., title = {...}, year = ...). Cite it with \\cite(key)."},
        {"bibliography", "\\bibliography", "The reference list: every cited entry, numbered in the order first cited."},
        {"toc", "\\toc", "The table of contents: every heading of this document, indented by depth."},
        {"abstract", "\\abstract(text)", "The document's abstract: prose over any number of lines, a blank line between paragraphs. Exports as each format's own abstract."},
        {"slide", "\\slide( ... )", "A slide: \\slide( on a line of its own, then the slide's content -- headings, lists, code, pictures, any blocks -- and a line holding just ) to end it. Its first heading is its title."},
        {"columns", "\\columns( ... )", "Columns side by side: \\columns( on a line of its own, then one \\column( ... ) per column, and a line holding just ) to end the row. The editor, its presentation view, the PDF and HTML exports (slides and documents) and LaTeX set them beside each other; the other formats write one after another."},
        {"column", "\\column( ... )", "One column of a \\columns( row: \\column( on a line of its own, its content -- lists, pictures, code, any blocks -- and a line holding just ) to end it. Columns share the width equally; \\column(40%, gives one its own."},
#define MEPML_BOX(name, Label, colour)                                                                                       \
    {name, "\\" name "(Title, ...)",                                                                                         \
     "A " name ", drawn as a titled box (" colour "): \\" name "(Title, on a line of its own, then its content -- prose, "  \
     "maths, lists, code, any blocks -- and a line holding just ) to close it; or \\" name "(Title, text) on one line. "     \
     "The title (before the first comma) may be left out. Exports as each format's own: a box in HTML and PDF, a "            \
     "heading in the others. Label: " Label "."}
        MEPML_BOX("definition", "Definition", "blue"),
        MEPML_BOX("theorem", "Theorem", "purple"),
        MEPML_BOX("lemma", "Lemma", "purple"),
        MEPML_BOX("proposition", "Proposition", "purple"),
        MEPML_BOX("corollary", "Corollary", "purple"),
        MEPML_BOX("fact", "Fact", "orange"),
        MEPML_BOX("example", "Example", "green"),
        MEPML_BOX("remark", "Remark", "teal"),
        MEPML_BOX("proof", "Proof", "grey, ending with a tombstone"),
        MEPML_BOX("note", "Note", "slate blue"),
        MEPML_BOX("tip", "Tip", "green"),
        MEPML_BOX("warning", "Warning", "amber"),
        MEPML_BOX("important", "Important", "red"),
#undef MEPML_BOX
        {"define", "\\define(name(params), template)", "A command of your own: \\name(a, b) becomes the template with #param (or #{param}, #1) replaced by the arguments -- the last parameter takes the rest of the call, commas and all. The template may choose by export with \\when(html, ...) \\otherwise(...) and write the export's own markup with \\raw(html, ...). Exports expand the calls; the editor shows them as written."},
    };
    return v;
}

const std::vector<Vocab> &CommandVocab() {
    static const std::vector<Vocab> v = {
        {"cite", "\\cite(key)", "A textual citation: Author (Year)."},
        {"citep", "\\citep(key)", "A parenthetical citation: (Author, Year)."},
        {"fn", "\\fn(text)", "A footnote, numbered in document order."},
        {"color", "\\color(name, text)", "Coloured text: a name (red, blue, ...) or #rrggbb."},
        {"f", "\\f(family, text)", "Text in another font family (serif, sans, mono, or a font name)."},
        {"fs", "\\fs(points, text)", "Text at a size in points (body text is 12)."},
        {"when", "\\when(formats, text)", "Text only in the exports named -- html, tex, pdf, beamer, slides, md, docx, office ... (spaces or | between them; !html for all but HTML; * for all). An \\otherwise(text) right after one or more \\when()s is used when none matched."},
        {"otherwise", "\\otherwise(text)", "After \\when(formats, ...): the text for every other export."},
        {"class", "\\class(name, text)", "Text a style sheet selects by name: `.name { color: ...; }` in the document's sheet styles it, here and in the exports. It has no look of its own."},
        {"boxed", "\\boxed(kind, Title,", "A box of a kind of your own (axiom, key-result ...), closed by a line holding just `)`. Its label and colours are the style sheet's: `box[kind=axiom]::label { content: \"Axiom\"; }`, `box[kind=axiom] { --accent: #e07a00; }`."},
        {"raw", "\\raw(formats, text)", "Text written as it is into the exports named -- HTML markup for html, LaTeX for tex/pdf/beamer, and so on -- and left out of the rest."},
    };
    return v;
}

const std::vector<Vocab> &MetaVocab() {
    static const std::vector<Vocab> v = {
        {"Title", "//? Title: text", "The document's title: drawn large in the header and used by every export."},
        {"Subtitle", "//? Subtitle: text", "A line under the title."},
        {"Lang", "//? Lang: en-GB", "The language the document is written in (a BCP 47 tag: en, en-GB, fr ...). Every export says so where it can -- a screen reader picks its voice by it."},
        {"Author", "//? Author: name", "The author, for the exports' metadata."},
        {"Date", "//? Date: text", "The date, for the exports' metadata."},
        {"Option", "//? Option: Name=value", "A document option: an integer, a decimal or a string (quote it to force a string)."},
        {"Export", "//? Export: pdf", "The format the Run button (the pane header's play button, <leader>rr, gr) exports to and opens: html (the default), pdf, beamer (the slides as a Beamer PDF), docx, odt, rtf, md, org, tex, txt, pptx or odp."},
        {"Type", "//? Type: presentation", "What the document is: document (the default) or presentation. A presentation's html, tex and pdf exports are a slideshow and a Beamer deck of its \\slide blocks, after a title slide from the header; pptx and odp decks work either way."},
        {"Exports", "//? Exports: results", "What the exports show of every code block: code, results, both (the default) or none. A block's own exports= (or echo=) wins. The editor always shows everything."},
        {"Style", "//? Style: file.mepss", "A style sheet for this document (docs/mepml-spec/style.md): rules such as `heading[level=1] { color: #0b5cad; }` that override mep's default look, here and in every export. Several Style lines apply in order."},
        {"Import", "//? Import: file.mepml", "Includes another mepml file: its options and other header keys are inherited (this file's own win) and its content is included here, in the order the header lists its imports."},
    };
    return v;
}

const std::vector<Vocab> &CodeOptionVocab() {
    static const std::vector<Vocab> v = {
        {"file", "file=path.png", "The block draws a figure into this file; after a run it is shown under the block, numbered and captioned."},
        {"eval", "eval=false", "eval=false (or no, never) stops the block from running."},
        {"exports", "exports=results", "What the exports show of this block: code, results, both or none (the document's Exports: header by default)."},
        {"echo", "echo=false", "echo=false (knitr's name): the exports show the block's results but not its code."},
        {"results", "results=html | markdown | terminal | exec | exec-gui | web", "results=html: the block prints HTML, kept as markup and drawn rendered. results=markdown (or md, asis): it prints Markdown, read as part of the document -- a table it prints is a table. results=terminal (a shell block): it runs as a program in a terminal inside its results. results=exec (any language): the block is a program -- compiled first when the language is -- run in a terminal inside its results that you can type into and stop. results=exec-gui: the same, for a program that opens a window: the window is shown inside the results. results=web: a web page, live in a browser window inside the results -- an html block is the page (d3 and the like); any other block is a program serving one (a Shiny app ...), opened at the first http address it prints. Stopping it keeps its picture as the results; an HTML export embeds an html block's page itself."},
        {"rows", "rows=16", "The height of an exec block's terminal, or of an exec-gui block's window, in rows (16 and 20 by default)."},
        {"cols", "cols=80", "The width of an exec block's terminal, or of an exec-gui block's window, in columns (the text width by default)."},
    };
    return v;
}

// A user command as it is called: \name(p1, p2).
std::string CommandSignature(const mepml::UserCommand &c) {
    std::string params;
    for (const std::string &p : c.params) params += (params.empty() ? "" : ", ") + p;
    return "\\" + c.name + "(" + params + ")";
}
// Where it is defined and the start of its template.
std::string CommandDoc(const mepml::UserCommand &c) {
    std::string t = "Your command, defined " + (c.origin.empty() ? std::string("on line ") : "in " + c.origin + ", line ") +
                    std::to_string(c.line + 1) + ".";
    std::string body = c.body;
    if (body.size() > 400) body = body.substr(0, 400) + " ...";
    return t + "\n\n" + body;
}
std::vector<Candidate> Cands(const std::vector<Vocab> &v, MepmlLspKind kind, const std::string &suffix) {
    std::vector<Candidate> out;
    for (const Vocab &e : v) out.push_back({e.name, "", kind, e.detail, e.doc, suffix});
    return out;
}

// Files in (the document's directory)/`sub`, as completions of `typed`.
std::vector<Candidate> PathCands(const MepmlLspOptions &opts, const std::string &typed, const std::set<std::string> &exts) {
    std::vector<Candidate> out;
    if (!opts.check_files || opts.doc_path.empty()) return out;
    const size_t slash = typed.rfind('/');
    const std::string sub = slash == std::string::npos ? "" : typed.substr(0, slash + 1);
    const std::string dir = sub.empty() ? DocDir(opts) : Resolve(opts, sub);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        const bool is_dir = it->is_directory(ec);
        const std::string ext = Lower(it->path().extension().string());
        if (!is_dir && !exts.empty() && !exts.count(ext)) continue;
        out.push_back({sub + name + (is_dir ? "/" : ""), name + (is_dir ? "/" : ""), is_dir ? MepmlLspKind::Folder : MepmlLspKind::File,
                       "", "", ""});
    }
    std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) { return a.text < b.text; });
    return out;
}

}  // namespace

std::vector<MepmlLspCompletionItem> MepmlLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                        const MepmlLspOptions &opts) {
    std::vector<MepmlLspCompletionItem> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const std::string &l = lines[static_cast<size_t>(line)];
    col = std::clamp(col, 0, Len(l));
    const std::string before = l.substr(0, static_cast<size_t>(col));
    const size_t ind = before.find_first_not_of(" \t");
    const std::string lead = ind == std::string::npos ? "" : before.substr(ind);

    // Inside a code block, only its fence line is mepml.
    const Document &doc = Analyze(lines, opts);
    if (const Block *b = OwnBlockAt(doc, line)) {
        if (b->kind == BlockKind::Code && line >= b->code_line_start && line <= b->code_line_end) return out;
    }

    // Code fence: ```lang or ```{lang, option=...
    if (lead.rfind("```", 0) == 0) {
        std::string rest = lead.substr(3);
        const bool braced = !rest.empty() && rest[0] == '{';
        if (braced) rest = rest.substr(1);
        const size_t comma = rest.rfind(',');
        if (braced && comma != std::string::npos) {
            const std::string typed = Trim(rest.substr(comma + 1));
            if (typed.find('=') != std::string::npos) return out;
            std::vector<Candidate> c = Cands(CodeOptionVocab(), MepmlLspKind::Property, "=");
            for (const OrgLspVocabEntry &e : OrgLspHeaderArgVocab()) c.push_back({e.name, "", MepmlLspKind::Property, e.detail, e.doc, "="});
            Offer(out, typed, col, c);
            return out;
        }
        std::vector<Candidate> c;
        for (const OrgLspVocabEntry &e : OrgLspBabelLangVocab()) c.push_back({e.name, "", MepmlLspKind::Module, e.detail, e.doc, ""});
        c.push_back({"mep-lua", "", MepmlLspKind::Module, "mep's own Lua", "Runs inside mep itself, with the mep.* API.", ""});
        c.push_back({"exec", "", MepmlLspKind::Module, "a program in a terminal",
                     "The body is a command line; C-c C-c runs it in a terminal inside the block's results (Enter types into it, C-c C-k stops it).", ""});
        c.push_back({"exec-gui", "", MepmlLspKind::Module, "a program's window",
                     "The body is a command line starting a program that opens a window; C-c C-c shows that window inside the block's results "
                     "(Enter or a click types into it, Ctrl-\\ comes back, C-c C-k stops it). Its last picture is kept as the results.",
                     ""});
        Offer(out, rest, col, c);
        return out;
    }

    // `//? Key:` metadata keys.
    if (lead.rfind("//?", 0) == 0) {
        const std::string typed = Trim(lead.substr(3));
        const size_t colon = typed.find(':');
        if (colon != std::string::npos && Lower(Trim(typed.substr(0, colon))) == "import") {
            // The path after `//? Import:`, as typed so far.
            size_t v = before.find(':', before.find("//?")) + 1;
            while (v < before.size() && (before[v] == ' ' || before[v] == '\t')) ++v;
            const std::string path = before.substr(std::min(v, before.size()));
            Offer(out, path, col, PathCands(opts, path, {".mepml", ".bib"}));
            return out;
        }
        // The value after `//? Export:` / `//? Type:`: the formats and kinds there are.
        if (colon != std::string::npos) {
            const std::string key = Lower(Trim(typed.substr(0, colon)));
            const std::string value = Trim(typed.substr(colon + 1));
            std::vector<Candidate> c;
            if (key == "export")
                for (const mepml::ExportFormat &f : mepml::ExportFormats()) c.push_back({f.name, "", MepmlLspKind::Value, "export format", f.doc, ""});
            else if (key == "type") {
                c.push_back({"document", "", MepmlLspKind::Value, "document type", "An ordinary document (the default).", ""});
                c.push_back({"presentation", "", MepmlLspKind::Value, "document type",
                             "A slide deck: its html is a slideshow and its tex and pdf a Beamer deck, of its \\slide blocks.", ""});
            }
            if (!c.empty()) Offer(out, value, col, c);
            return out;
        }
        if (lead.size() > 3) Offer(out, typed, col, Cands(MetaVocab(), MepmlLspKind::Field, ": "));
        return out;
    }

    // `// KEYWORD:` callouts (only once a capital has been typed: a
    // comment line's first word is usually just prose).
    if (lead.rfind("//", 0) == 0) {
        const std::string typed = Trim(lead.substr(2));
        if (!typed.empty() && std::isupper(static_cast<unsigned char>(typed[0])) != 0 && typed.find(' ') == std::string::npos &&
            typed.find(':') == std::string::npos) {
            std::vector<Candidate> c;
            for (const std::string &k : mepml::CalloutKeywords()) c.push_back({k, "", MepmlLspKind::Keyword, "callout", "A // " + k + ": callout box.", ": "});
            Offer(out, typed, col, c);
        }
        return out;
    }

    // The innermost unclosed `(` (or the older `{`) before the cursor: a
    // command's or directive's argument being typed.
    std::vector<size_t> groups;
    for (size_t k = 0; k < before.size(); ++k) {
        const char c = before[k];
        if (c == '\\' && k + 1 < before.size() && std::isalpha(static_cast<unsigned char>(before[k + 1])) == 0) ++k;
        else if (c == '(' || c == '{') groups.push_back(k);
        else if ((c == ')' || c == '}') && !groups.empty() && before[groups.back()] == (c == ')' ? '(' : '{')) groups.pop_back();
    }
    if (!groups.empty()) {
        const size_t brace = groups.back();
        const bool paren = before[brace] == '(';
        size_t n = brace;
        while (n > 0 && std::isalpha(static_cast<unsigned char>(before[n - 1])) != 0) --n;
        const std::string name = before.substr(n, brace - n);
        const char sigil = n > 0 ? before[n - 1] : 0;
        const std::string typed = before.substr(brace + 1);
        const bool two_args = name == "color" || name == "f" || name == "fs";
        // The text of \color(name, text) / \f(...) / \fs(...), or the second
        // group of \color{}{}: nothing to offer.
        if (!paren && sigil == '}') return out;
        if (paren && two_args && sigil == '\\' && typed.find(',') != std::string::npos) return out;
        if (sigil == '\\' && (name == "cite" || name == "citep")) {
            std::vector<Candidate> c;
            for (const auto &kv : doc.citations)
                c.push_back({kv.first, "", MepmlLspKind::Reference, mepml::CiteLabel(doc, kv.first, false), CitationSummary(doc, kv.first), ""});
            Offer(out, typed, col, c);
            return out;
        }
        const std::string next = paren ? ", " : "";  // after the argument: on to the text
        // \when(formats, ...) / \raw(formats, ...): the export names.
        if (paren && sigil == '\\' && (name == "when" || name == "raw")) {
            if (typed.find(',') != std::string::npos) return out;
            const size_t word = typed.find_last_of(" |!");
            std::vector<Candidate> c;
            for (const std::string &t : mepml::KnownFormatTags()) c.push_back({t, "", MepmlLspKind::Value, "export", "", ""});
            Offer(out, word == std::string::npos ? typed : typed.substr(word + 1), col, c);
            return out;
        }
        if (sigil == '\\' && name == "color") {
            std::vector<Candidate> c;
            for (const std::string &cn : mepml::ColorNames()) {
                std::uint32_t rgb = 0;
                mepml::ParseColor(cn, &rgb);
                char hex[8];
                std::snprintf(hex, sizeof hex, "#%06x", rgb);
                c.push_back({cn, "", MepmlLspKind::Color, hex, "", next});
            }
            Offer(out, typed, col, c);
            return out;
        }
        if (sigil == '\\' && name == "f") {
            std::vector<Candidate> c;
            for (const char *f : {"serif", "sans", "mono", "Helvetica", "Times", "Courier", "Georgia", "Palatino"})
                c.push_back({f, "", MepmlLspKind::Value, "font family", "", next});
            Offer(out, typed, col, c);
            return out;
        }
        if (sigil == '\\' && name == "fs") {
            std::vector<Candidate> c;
            for (const char *f : {"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "32"})
                c.push_back({f, "", MepmlLspKind::Value, "points", "", next});
            Offer(out, typed, col, c);
            return out;
        }
        const bool directive = sigil == (paren ? '\\' : '@');
        if (directive && name == "image") {
            Offer(out, typed, col, PathCands(opts, typed, {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg", ".webp"}));
            return out;
        }
        if (directive && name == "import") {
            Offer(out, typed, col, PathCands(opts, typed, {".mepml", ".bib"}));
            return out;
        }
        if (!paren) return out;
        // Any other parenthesis (prose, a footnote's text): what follows.
    }

    // A link's target: `[text|` ...
    {
        const size_t open = before.rfind('[');
        const size_t bar = open == std::string::npos ? open : before.find('|', open);
        if (bar != std::string::npos && before.find(']', bar) == std::string::npos) {
            const std::string typed = before.substr(bar + 1);
            std::vector<Candidate> c;
            for (const Heading &h : Headings(doc, lines)) c.push_back({"#" + h.slug, "", MepmlLspKind::Reference, h.title, "", ""});
            if (typed.empty() || typed[0] != '#') {
                std::vector<Candidate> files = PathCands(opts, typed, {});
                c.insert(c.end(), files.begin(), files.end());
            }
            Offer(out, typed, col, c);
            return out;
        }
    }

    // A directive's candidates: \image(, \toc, ...
    auto directives = [] {
        std::vector<Candidate> c;
        for (const Vocab &v : DirectiveVocab()) {
            const std::string n = v.name;
            c.push_back({n, "\\" + n, MepmlLspKind::Keyword, v.detail, v.doc, (n == "bibliography" || n == "toc") ? "" : "("});
        }
        return c;
    };

    // A backslash command being typed: \ci -> \cite(, and at the start of
    // a line a directive too: \im -> \image(
    {
        size_t n = before.size();
        while (n > 0 && std::isalpha(static_cast<unsigned char>(before[n - 1])) != 0) --n;
        if (n > 0 && before[n - 1] == '\\') {
            std::vector<Candidate> c = Cands(CommandVocab(), MepmlLspKind::Function, "(");
            for (const auto &kv : doc.commands) c.push_back({kv.first, "", MepmlLspKind::Function, CommandSignature(kv.second), CommandDoc(kv.second), "("});
            for (Candidate &k : c) k.label = "\\" + k.text;
            if (ind != std::string::npos && n - 1 == ind) {
                const std::vector<Candidate> d = directives();
                c.insert(c.end(), d.begin(), d.end());
            }
            Offer(out, before.substr(n), col, c);
            return out;
        }
    }

    // The older `@` spelling at the start of a line: @im -> \image(, the
    // `@` rewritten.
    if (!lead.empty() && lead[0] == '@') {
        const std::string typed = lead.substr(1);
        bool letters = true;
        for (char c : typed) letters = letters && std::isalpha(static_cast<unsigned char>(c)) != 0;
        if (letters) {
            const size_t from = out.size();
            const std::vector<Candidate> c = directives();
            Offer(out, typed, col, c);
            for (size_t k = from; k < out.size(); ++k) {
                // `typed` is all letters, so the item replaces all of it.
                out[k].insert_text = "\\" + out[k].insert_text;
                out[k].replace_start = static_cast<int>(ind);
            }
        }
    }
    return out;
}

// ===========================================================================
// Hover
// ===========================================================================

namespace {

const char *InlineName(InlineKind k) {
    switch (k) {
        case InlineKind::Bold: return "Bold: *text*";
        case InlineKind::Italic: return "Italic: ~text~";
        case InlineKind::Underline: return "Underline: _text_";
        case InlineKind::Superscript: return "Superscript: ^text^";
        case InlineKind::Subscript: return "Subscript: ,,text,,";
        case InlineKind::Small: return "Small text: <text>";
        case InlineKind::Big: return "Big text: >text<";
        case InlineKind::Mono: return "Monospaced: |text| (formatting still works inside)";
        case InlineKind::Highlight: return "Highlighted: =text=";
        case InlineKind::Strike: return "Struck through: -text-";
        case InlineKind::Insert: return "Inserted: +text+";
        case InlineKind::Delete: return "Deleted: !text!";
        case InlineKind::Verbatim: return "Verbatim: `text` (no formatting inside)";
        case InlineKind::Math: return "Maths (TeX): $...$ or \\(...\\)";
        case InlineKind::Raw: return "Raw text: written as it is into the exports named, left out of the rest";
        default: return nullptr;
    }
}

std::string VocabDoc(const std::vector<Vocab> &v, const std::string &name) {
    for (const Vocab &e : v)
        if (Lower(e.name) == Lower(name)) return std::string(e.detail) + "\n\n" + e.doc;
    return "";
}

// Hovering a call of `name`.
std::string CommandHover(const Document &doc, const std::string &name) {
    if (name == "when" || name == "otherwise") return VocabDoc(CommandVocab(), name);
    auto it = doc.commands.find(name);
    if (it == doc.commands.end()) return "Unknown command \\" + name + ": no \\define(" + name + "(...), ...) for it";
    return CommandSignature(it->second) + "\n\n" + CommandDoc(it->second);
}

}  // namespace

MepmlLspHoverInfo MepmlLspHover(const std::vector<std::string> &lines, int line, int col, const MepmlLspOptions &opts) {
    MepmlLspHoverInfo h;
    if (line < 0 || line >= static_cast<int>(lines.size())) return h;
    const std::string &l = lines[static_cast<size_t>(line)];
    const Document &doc = Analyze(lines, opts);
    const Block *b = OwnBlockAt(doc, line);
    if (!b) return h;
    auto found = [&](int cs, int ce, const std::string &text) {
        h.found = true;
        h.line = line;
        h.col_start = cs;
        h.col_end = ce;
        h.text = text;
        return h;
    };

    if (const Inline *x = InlineAt(*b, line, col)) {
        const Pos p = At(*b, x->start), q = At(*b, x->end);
        const int cs = p.line == line ? p.col : 0, ce = q.line == line ? q.col : Len(l);
        switch (x->kind) {
            case InlineKind::Cite:
            case InlineKind::CiteP: {
                auto it = doc.citations.find(x->text);
                if (it == doc.citations.end()) return found(cs, ce, "Unknown citation key '" + x->text + "'");
                std::string t = mepml::CiteLabel(doc, x->text, x->kind == InlineKind::CiteP) + "\n\n" + CitationSummary(doc, x->text) + "\n";
                for (const std::string &f : it->second.field_order) t += "\n" + f + " = " + it->second.fields.at(f);
                return found(cs, ce, t);
            }
            case InlineKind::Footnote:
                return found(cs, ce, "Footnote " + std::to_string(x->number) + "\n\n" + mepml::InlinePlainText(x->children));
            case InlineKind::Link: {
                std::string t = "Link to " + x->arg;
                if (!x->arg.empty() && x->arg[0] == '#') {
                    const std::vector<Heading> hs = Headings(doc, lines);
                    if (const Heading *hd = HeadingForAnchor(hs, x->arg.substr(1)))
                        t += "\n\nHeading \"" + hd->title + "\" (line " + std::to_string(hd->line + 1) + ")";
                    else
                        t += "\n\nNo heading has this anchor.";
                } else if (!IsExternal(x->arg)) {
                    const std::string path = Resolve(opts, FilePart(x->arg));
                    t += "\n\n" + path + (opts.check_files && !FileExists(path) ? " (does not exist)" : "");
                }
                return found(cs, ce, t);
            }
            case InlineKind::Color: {
                std::uint32_t rgb = 0;
                if (!mepml::ParseColor(x->arg, &rgb)) return found(cs, ce, "Unknown colour '" + x->arg + "'");
                char hex[8];
                std::snprintf(hex, sizeof hex, "#%06x", rgb);
                return found(cs, ce, "Colour " + x->arg + " = " + hex);
            }
            case InlineKind::Font: return found(cs, ce, "Font family: " + x->arg);
            case InlineKind::FontSize: return found(cs, ce, "Font size: " + x->arg + " pt (body text is 12)");
            case InlineKind::Math: return found(cs, ce, std::string("Maths (TeX)") + (x->alt.empty() ? "" : "\n\nAlt text: " + x->alt));
            case InlineKind::Command: return found(cs, ce, CommandHover(doc, x->arg));
            case InlineKind::Raw: return found(cs, ce, "Raw text for: " + x->arg + "\n\nWritten as it is into those exports, left out of the rest.");
            default:
                if (const char *name = InlineName(x->kind)) return found(cs, ce, name);
        }
    }

    const std::string lead = Trim(l);
    const int ind = static_cast<int>(l.find_first_not_of(" \t") == std::string::npos ? 0 : l.find_first_not_of(" \t"));
    switch (b->kind) {
        case BlockKind::Heading: {
            if (line != b->line_start) break;
            const std::string title = mepml::InlinePlainText(b->inlines);
            const std::string slug = mepml::HeadingSlug(title);
            int links = 0;
            ForEachOwnBlock(doc, [&](const Block &o) {
                ForEachInline(o, [&](const Inline &x) {
                    if (x.kind == InlineKind::Link && !x.arg.empty() && x.arg[0] == '#' && mepml::HeadingSlug(x.arg.substr(1)) == slug) ++links;
                });
            });
            return found(0, Len(l), "Heading, level " + std::to_string(b->level) + "\n\nAnchor: #" + slug + "\nLinks to it: " + std::to_string(links));
        }
        case BlockKind::Image:
        case BlockKind::Import: {
            if (line != b->line_start) break;
            const std::string path = Resolve(opts, b->value);
            std::string t = VocabDoc(DirectiveVocab(), b->kind == BlockKind::Image ? "image" : "import") + "\n\n" + path;
            if (opts.check_files && !FileExists(path)) t += " (does not exist)";
            return found(ind, Len(l), t);
        }
        case BlockKind::Citation: {
            if (line != b->line_start) break;
            int uses = 0;
            ForEachOwnBlock(doc, [&](const Block &o) {
                ForEachInline(o, [&](const Inline &x) {
                    if ((x.kind == InlineKind::Cite || x.kind == InlineKind::CiteP) && x.text == b->value) ++uses;
                });
            });
            return found(ind, Len(l), "Citation '" + b->value + "'\n\n" + CitationSummary(doc, b->value) + "\n\nCited " + std::to_string(uses) +
                                          (uses == 1 ? " time" : " times"));
        }
        case BlockKind::Define: {
            if (line != b->line_start) break;
            auto it = doc.commands.find(b->keyword);
            if (it == doc.commands.end()) return found(ind, Len(l), VocabDoc(DirectiveVocab(), "define"));
            int uses = 0;
            ForEachOwnBlock(doc, [&](const Block &o) {
                if (o.kind == BlockKind::Command && o.keyword == b->keyword) ++uses;
                ForEachInline(o, [&](const Inline &x) {
                    if (x.kind == InlineKind::Command && x.arg == b->keyword) ++uses;
                });
            });
            return found(ind, Len(l), CommandHover(doc, b->keyword) + "\n\nCalled " + std::to_string(uses) + (uses == 1 ? " time" : " times"));
        }
        case BlockKind::Command:
            if (line != b->line_start) break;
            return found(ind, Len(l), CommandHover(doc, b->keyword));
        case BlockKind::Raw:
            if (line != b->line_start) break;
            return found(ind, Len(l), "Raw text for: " + b->lang + "\n\n" + VocabDoc(CommandVocab(), "raw"));
        case BlockKind::Bibliography: return found(ind, Len(l), VocabDoc(DirectiveVocab(), "bibliography"));
        case BlockKind::TableOfContents: return found(ind, Len(l), VocabDoc(DirectiveVocab(), "toc"));
        case BlockKind::Abstract: {
            if (line != b->line_start) break;
            const size_t paras = b->paragraph_starts.size();
            return found(ind, Len(l), VocabDoc(DirectiveVocab(), "abstract") + "\n\n" + std::to_string(paras) +
                                          (paras == 1 ? " paragraph" : " paragraphs"));
        }
        case BlockKind::BoxBegin:
        case BlockKind::BoxEnd: {
            // A box's heading and its extent, from either end.
            const Block *begin = b;
            int end_line = b->box_closed ? b->line_end : -1;
            std::vector<const Block *> open;
            for (const Block &o : doc.blocks) {
                if (!o.origin.empty()) continue;
                if (o.kind == BlockKind::BoxBegin && !o.box_closed) open.push_back(&o);
                if (o.kind == BlockKind::BoxEnd && !open.empty()) {
                    if (&o == b) begin = open.back();
                    if (open.back() == b) end_line = o.line_end;
                    open.pop_back();
                }
            }
            if (b->kind == BlockKind::BoxEnd) end_line = b->line_end;
            std::string t = mepml::BoxHeading(*begin);
            if (end_line >= 0) t += "\n\nLines " + std::to_string(begin->line_start + 1) + "-" + std::to_string(end_line + 1);
            else t += "\n\nNot closed: end it with a line holding just )";
            return found(ind, Len(l), t + "\n\n" + VocabDoc(DirectiveVocab(), begin->keyword));
        }
        case BlockKind::LayoutBegin:
        case BlockKind::LayoutEnd: return found(ind, Len(l), VocabDoc(DirectiveVocab(), b->keyword));
        case BlockKind::SlideBegin:
        case BlockKind::SlideEnd: {
            std::string t = "Slide " + std::to_string(b->level);
            for (const mepml::Slide &sl : mepml::Slides(doc, static_cast<int>(lines.size())))
                if (sl.number == b->level) {
                    if (!sl.title.empty()) t += ": " + sl.title;
                    t += "\n\nLines " + std::to_string(sl.line_start + 1) + "-" + std::to_string(sl.line_end + 1);
                }
            return found(ind, Len(l), t + "\n\n" + VocabDoc(DirectiveVocab(), "slide"));
        }
        case BlockKind::Meta: {
            if (Lower(b->keyword) == "import") {
                // As the expander resolves it, so it matches the blocks' origin.
                const std::string path = opts.doc_path.empty() ? Trim(b->value) : mepml::ResolvePath(opts.doc_path, Trim(b->value));
                std::string t = VocabDoc(MetaVocab(), "import") + "\n\n" + path;
                if (opts.check_files && !FileExists(path)) return found(ind, Len(l), t + " (does not exist)");
                // What it brings: its blocks follow this line in doc.blocks.
                int blocks = 0;
                std::set<std::string> cites;
                for (const Block &o : doc.blocks) {
                    if (o.origin != path) continue;
                    if (o.kind == BlockKind::Citation) cites.insert(o.value);
                    else if (o.kind != BlockKind::Meta && o.kind != BlockKind::Comment) ++blocks;
                }
                std::vector<std::string> opts_here;
                std::vector<std::string> lines_of;
                if (opts.check_files && ReadFileLines(path, &lines_of)) {
                    for (const mepml::Option &o : mepml::Parse(lines_of).options) {
                        const mepml::Option *mine = doc.FindOption(o.name);
                        opts_here.push_back(o.name + " = " + o.value.s + (mine && mine->value.s != o.value.s ? " (overridden here)" : ""));
                    }
                }
                if (!opts_here.empty()) {
                    t += "\n\nOptions:";
                    for (const std::string &o : opts_here) t += "\n  " + o;
                }
                t += "\n\nContent: " + std::to_string(blocks) + (blocks == 1 ? " block" : " blocks") + ", " + std::to_string(cites.size()) +
                     (cites.size() == 1 ? " citation" : " citations");
                return found(ind, Len(l), t);
            }
            std::string t = VocabDoc(MetaVocab(), b->keyword);
            if (Lower(b->keyword) == "option" && !b->options.empty()) {
                const mepml::Value &v = b->options[0].value;
                t += std::string("\n\n") + b->options[0].name + " is " +
                     (v.kind == mepml::ValueKind::Int ? "an integer" : v.kind == mepml::ValueKind::Double ? "a decimal" : "a string");
            }
            if (t.empty()) t = "Document metadata: " + b->keyword + " = " + b->value;
            return found(ind, Len(l), t);
        }
        case BlockKind::Code: {
            if (line >= b->code_line_start && line <= b->code_line_end) break;  // the code is its own language's
            std::string t = "Code block" + (b->lang.empty() ? std::string(" (no language: it cannot run)") : ": " + b->lang);
            for (const mepml::Option &o : b->options) t += "\n" + o.name + " = " + o.value.s;
            bool in_terminal = Lower(b->lang) == "exec" || Lower(b->lang) == "executable";
            bool in_window = Lower(b->lang) == "exec-gui" || Lower(b->lang) == "gui";
            bool on_web = false;
            for (const mepml::Option &o : b->options) {
                const std::string n = Lower(o.name), v = Lower(o.value.s);
                if ((n == "results" || n == "output") && (v == "exec" || v == "terminal")) in_terminal = true;
                if ((n == "results" || n == "output") && v == "exec-gui") in_window = true;
                if ((n == "results" || n == "output") && (v == "web" || v == "app")) on_web = true;
            }
            if (on_web)
                t += Lower(b->lang) == "html"
                         ? "\n\nA web page: C-c C-c opens it in a browser window inside its results (a click types into it, "
                           "C-c C-k stops it, keeping its picture). An HTML export embeds the page itself."
                         : "\n\nA web app: C-c C-c runs it and opens the first http address it prints in a browser window inside "
                           "its results (a click types into it, C-c C-k stops it and the server, keeping its picture).";
            else if (in_window)
                t += "\n\nC-c C-c runs it and shows its window inside its results: Enter or a click types into it, Ctrl-\\ returns, "
                     "C-c C-k stops it. Its last picture (and what it printed) is kept as the results.";
            else if (in_terminal)
                t += "\n\nC-c C-c runs it in a terminal inside its results: Enter types into it, Ctrl-\\ Ctrl-N returns, C-c C-k stops it. "
                     "Its final screen is kept as the results.";
            else
                t += "\n\nRun it with C-c C-c; its output is written between // result_begin: and // result_end.";
            return found(ind, Len(l), t);
        }
        case BlockKind::Callout:
            if (line == b->line_start) return found(ind, Len(l), "Callout: " + b->keyword);
            break;
        default: break;
    }
    for (const char *name : {"caption", "alttext"})
        if (lead.rfind(std::string("\\") + name, 0) == 0 || lead.rfind(std::string("@") + name, 0) == 0)
            return found(ind, Len(l), VocabDoc(DirectiveVocab(), name));
    return h;
}

// ===========================================================================
// Definition, references, rename
// ===========================================================================

namespace {

// What the cursor is on, for the reference machinery.
struct Target {
    enum Kind { None, Citation, Heading } kind = None;
    std::string key;  // citation key / heading slug
};

Target TargetAt(const Document &doc, const std::vector<std::string> &lines, int line, int col) {
    Target t;
    const Block *b = OwnBlockAt(doc, line);
    if (!b) return t;
    if (const Inline *x = InlineAt(*b, line, col)) {
        if (x->kind == InlineKind::Cite || x->kind == InlineKind::CiteP) return {Target::Citation, x->text};
        if (x->kind == InlineKind::Link && !x->arg.empty() && x->arg[0] == '#') {
            const std::vector<Heading> hs = Headings(doc, lines);
            if (const Heading *h = HeadingForAnchor(hs, x->arg.substr(1))) return {Target::Heading, h->slug};
        }
    }
    if (b->kind == BlockKind::Citation && line == b->line_start) return {Target::Citation, b->value};
    if (b->kind == BlockKind::Heading && line == b->line_start) return {Target::Heading, mepml::HeadingSlug(mepml::InlinePlainText(b->inlines))};
    return t;
}

}  // namespace

MepmlLspLocation MepmlLspDefinition(const std::vector<std::string> &lines, int line, int col, const MepmlLspOptions &opts) {
    MepmlLspLocation loc;
    if (line < 0 || line >= static_cast<int>(lines.size())) return loc;
    const Document &doc = Analyze(lines, opts);
    const Block *b = OwnBlockAt(doc, line);
    if (!b) return loc;
    if (const Inline *x = InlineAt(*b, line, col)) {
        if (x->kind == InlineKind::Cite || x->kind == InlineKind::CiteP) {
            for (const CitationDef &c : CitationDefs(doc, lines)) {
                if (c.key != x->text) continue;
                loc.found = true;
                loc.path = c.origin;
                loc.line = c.line;
                loc.col_start = c.col_start;
                loc.col_end = c.col_end;
                return loc;
            }
            return loc;
        }
        if (x->kind == InlineKind::Link) {
            if (!x->arg.empty() && x->arg[0] == '#') {
                const std::vector<Heading> hs = Headings(doc, lines);
                if (const Heading *h = HeadingForAnchor(hs, x->arg.substr(1))) {
                    loc.found = true;
                    loc.line = h->line;
                    loc.col_start = h->title_col;
                    loc.col_end = h->title_end;
                }
                return loc;
            }
            if (!IsExternal(x->arg) && !FilePart(x->arg).empty()) {
                loc.found = true;
                loc.path = Resolve(opts, FilePart(x->arg));
                return loc;
            }
            return loc;
        }
    }
    if (b->kind == BlockKind::Meta && Lower(b->keyword) == "import" && !Trim(b->value).empty()) {
        loc.found = true;
        loc.path = Resolve(opts, Trim(b->value));
        return loc;
    }
    if ((b->kind == BlockKind::Image || b->kind == BlockKind::Import) && line == b->line_start && !b->value.empty()) {
        loc.found = true;
        loc.path = Resolve(opts, b->value);
        return loc;
    }
    if (b->kind == BlockKind::Code) {
        for (const auto &img : b->result_images) {
            if (img.first != line) continue;
            loc.found = true;
            loc.path = Resolve(opts, img.second);
            return loc;
        }
    }
    return loc;
}

MepmlLspReferenceSet MepmlLspReferences(const std::vector<std::string> &lines, int line, int col,
                                        const MepmlLspOptions &opts) {
    MepmlLspReferenceSet set;
    if (line < 0 || line >= static_cast<int>(lines.size())) return set;
    const Document &doc = Analyze(lines, opts);
    const Target t = TargetAt(doc, lines, line, col);
    if (t.kind == Target::None) return set;
    set.found = true;
    if (t.kind == Target::Citation) {
        for (const CitationDef &c : CitationDefs(doc, lines)) {
            if (c.key != t.key) continue;
            if (c.origin.empty()) set.refs.push_back({c.line, c.col_start, c.col_end, true});
            else set.rename_blocked_reason = "'" + t.key + "' is defined in " + c.origin;
        }
        ForEachOwnBlock(doc, [&](const Block &b) {
            ForEachInline(b, [&](const Inline &x) {
                if ((x.kind == InlineKind::Cite || x.kind == InlineKind::CiteP) && x.text == t.key) {
                    const KeyRange r = CiteKeyRange(b, x);
                    set.refs.push_back({r.line, r.col_start, r.col_end, false});
                }
            });
        });
    } else {
        set.is_heading = true;
        for (const Heading &h : Headings(doc, lines))
            if (h.slug == t.key) set.refs.push_back({h.line, h.title_col, h.title_end, true});
        ForEachOwnBlock(doc, [&](const Block &b) {
            ForEachInline(b, [&](const Inline &x) {
                if (x.kind == InlineKind::Link && !x.arg.empty() && x.arg[0] == '#' && mepml::HeadingSlug(x.arg.substr(1)) == t.key) {
                    const KeyRange r = LinkUrlRange(b, x);
                    set.refs.push_back({r.line, r.col_start, r.col_end, false});
                }
            });
        });
        if (set.refs.size() > 1 && std::count_if(set.refs.begin(), set.refs.end(), [](const MepmlLspReference &r) { return r.is_definition; }) > 1)
            set.rename_blocked_reason = "more than one heading has the anchor #" + t.key;
    }
    std::sort(set.refs.begin(), set.refs.end(), [](const MepmlLspReference &a, const MepmlLspReference &b) {
        return a.line != b.line ? a.line < b.line : a.col_start < b.col_start;
    });
    return set;
}

std::vector<MepmlLspTextEdit> MepmlLspRename(const std::vector<std::string> &lines, int line, int col,
                                             const std::string &new_name, const MepmlLspOptions &opts,
                                             std::string *error) {
    std::vector<MepmlLspTextEdit> edits;
    const MepmlLspReferenceSet set = MepmlLspReferences(lines, line, col, opts);
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return std::vector<MepmlLspTextEdit>();
    };
    if (!set.found || set.refs.empty()) return fail("Nothing to rename here");
    if (!set.rename_blocked_reason.empty()) return fail("Cannot rename: " + set.rename_blocked_reason);
    const std::string name = Trim(new_name);
    if (name.empty()) return fail("The new name is empty");
    if (!set.is_heading) {
        for (char c : name)
            if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == '{' || c == '}' || c == ',')
                return fail("A citation key cannot contain spaces, braces or commas");
    }
    for (const MepmlLspReference &r : set.refs) {
        MepmlLspTextEdit e;
        e.start_line = e.end_line = r.line;
        e.start_col = r.col_start;
        e.end_col = r.col_end;
        // A heading's title takes the new name; each link to it, the new
        // name's anchor.
        e.new_text = set.is_heading && !r.is_definition ? "#" + mepml::HeadingSlug(name) : name;
        edits.push_back(e);
    }
    return edits;
}

// ===========================================================================
// Symbols and folds
// ===========================================================================

std::vector<MepmlLspSymbol> MepmlLspSymbols(const std::vector<std::string> &lines) {
    const Document &doc = ParsePlain(lines);
    const std::vector<std::string> labels = mepml::BlockLabels(doc);
    const std::vector<mepml::Slide> slides = mepml::Slides(doc, static_cast<int>(lines.size()));
    // Each box's last line: its closing `)`, or its own for one closed there.
    std::map<const Block *, int> box_ends;
    {
        std::vector<const Block *> open;
        for (const Block &b : doc.blocks) {
            if (b.kind == BlockKind::BoxBegin && b.box_closed) box_ends[&b] = b.line_end;
            else if (b.kind == BlockKind::BoxBegin) open.push_back(&b);
            else if (b.kind == BlockKind::BoxEnd && !open.empty()) {
                box_ends[open.back()] = b.line_end;
                open.pop_back();
            }
        }
    }
    std::vector<MepmlLspSymbol> out;
    std::vector<int> open;  // indices of the headings enclosing the current line, by depth
    const int n = static_cast<int>(lines.size());
    auto close_to = [&](int level, int before_line) {
        while (!open.empty() && out[static_cast<size_t>(open.back())].line_start >= 0) {
            MepmlLspSymbol &s = out[static_cast<size_t>(open.back())];
            const int lvl = std::stoi(s.detail.substr(1));
            if (lvl < level) break;
            int end = before_line - 1;
            while (end > s.line_start && Trim(lines[static_cast<size_t>(end)]).empty()) --end;
            s.line_end = std::max(s.line_start, end);
            open.pop_back();
        }
    };
    for (size_t i = 0; i < doc.blocks.size(); ++i) {
        const Block &b = doc.blocks[i];
        if (b.kind == BlockKind::Heading) {
            close_to(b.level, b.line_start);
            MepmlLspSymbol s;
            s.name = mepml::InlinePlainText(b.inlines);
            if (s.name.empty()) s.name = "(untitled)";
            s.detail = "h" + std::to_string(b.level);
            s.kind = MepmlLspSymbolKind::Struct;
            s.line_start = b.line_start;
            s.line_end = n - 1;
            s.sel_line = b.line_start;
            s.sel_col_start = mepml::LineHeadingMarkupLen(lines[static_cast<size_t>(b.line_start)]);
            s.sel_col_end = Len(lines[static_cast<size_t>(b.line_start)]);
            s.parent = open.empty() ? -1 : open.back();
            out.push_back(s);
            open.push_back(static_cast<int>(out.size()) - 1);
            continue;
        }
        MepmlLspSymbol s;
        s.line_start = b.line_start;
        s.line_end = b.line_end;
        s.sel_line = b.line_start;
        s.sel_col_start = 0;
        s.sel_col_end = Len(lines[static_cast<size_t>(b.line_start)]);
        s.parent = open.empty() ? -1 : open.back();
        const std::string label = labels[i];
        if (b.kind == BlockKind::Code) {
            s.kind = MepmlLspSymbolKind::Function;
            s.name = b.caption.empty() ? (b.lang.empty() ? "code" : b.lang) + " block" : b.caption;
            s.detail = label.empty() ? b.lang : label;
            if (b.code_line_start > 0) s.sel_line = b.code_line_start - 1;  // the fence
            s.sel_col_end = Len(lines[static_cast<size_t>(s.sel_line)]);
        } else if (b.kind == BlockKind::Image) {
            s.kind = MepmlLspSymbolKind::Object;
            s.name = b.caption.empty() ? b.value : b.caption;
            s.detail = label.empty() ? "figure" : label;
        } else if (b.kind == BlockKind::Table) {
            s.kind = MepmlLspSymbolKind::Array;
            s.name = b.caption.empty() ? "table" : b.caption;
            s.detail = label.empty() ? "table" : label;
        } else if (b.kind == BlockKind::Citation) {
            s.kind = MepmlLspSymbolKind::Key;
            s.name = b.value;
            s.detail = "citation";
        } else if (b.kind == BlockKind::Abstract) {
            s.kind = MepmlLspSymbolKind::Namespace;
            s.name = "Abstract";
            s.detail = "abstract";
        } else if (b.kind == BlockKind::BoxBegin) {
            s.kind = MepmlLspSymbolKind::Object;
            s.name = mepml::BoxHeading(b);
            s.detail = b.keyword;
            auto end = box_ends.find(&b);
            if (end != box_ends.end()) s.line_end = end->second;
        } else if (b.kind == BlockKind::SlideBegin) {
            s.kind = MepmlLspSymbolKind::Namespace;
            s.name = "Slide " + std::to_string(b.level);
            s.detail = "slide";
            for (const mepml::Slide &sl : slides)
                if (sl.number == b.level) {
                    if (!sl.title.empty()) s.name += ": " + sl.title;
                    s.line_end = sl.line_end;
                }
        } else {
            continue;
        }
        out.push_back(s);
    }
    close_to(1, n);
    return out;
}

std::vector<MepmlLspFold> MepmlLspFolds(const std::vector<std::string> &lines) {
    const Document &doc = ParsePlain(lines);
    const int n = static_cast<int>(lines.size());
    std::vector<MepmlLspFold> out;
    auto add = [&](int a, int b, const char *kind) {
        if (b > a) out.push_back({a, b, kind});
    };
    std::vector<const Block *> heads;
    for (const Block &b : doc.blocks)
        if (b.kind == BlockKind::Heading) heads.push_back(&b);
    for (size_t k = 0; k < heads.size(); ++k) {
        int end = n - 1;
        for (size_t j = k + 1; j < heads.size(); ++j) {
            if (heads[j]->level <= heads[k]->level) {
                end = heads[j]->line_start - 1;
                break;
            }
        }
        while (end > heads[k]->line_start && Trim(lines[static_cast<size_t>(end)]).empty()) --end;
        add(heads[k]->line_start, end, "");
    }
    // Document headers: runs of `//?` lines on consecutive rows.
    int run_start = -1, run_end = -1;
    for (const Block &b : doc.blocks) {
        if (b.kind != BlockKind::Meta) continue;
        if (run_end >= 0 && b.line_start == run_end + 1) {
            run_end = b.line_end;
        } else {
            if (run_start >= 0) add(run_start, run_end, "");
            run_start = b.line_start;
            run_end = b.line_end;
        }
    }
    if (run_start >= 0) add(run_start, run_end, "");
    for (const mepml::Slide &sl : mepml::Slides(doc, n)) add(sl.line_start, sl.line_end, "");
    {
        std::vector<const Block *> open;  // boxes and columns
        for (const Block &b : doc.blocks) {
            if (b.kind == BlockKind::BoxBegin && b.box_closed) add(b.line_start, b.line_end, "");
            else if (b.kind == BlockKind::BoxBegin || b.kind == BlockKind::LayoutBegin) open.push_back(&b);
            else if ((b.kind == BlockKind::BoxEnd || b.kind == BlockKind::LayoutEnd) && !open.empty()) {
                add(open.back()->line_start, b.line_end, "");
                open.pop_back();
            }
        }
    }
    for (const Block &b : doc.blocks) {
        if (b.kind == BlockKind::Comment) add(b.line_start, b.line_end, "comment");
        else if (b.kind == BlockKind::Code && b.result_line_start >= 0) {
            // The code and its results fold separately, as in the editor.
            add(b.line_start, b.result_line_start - 1, "");
            add(b.result_line_start, b.result_line_end, "");
        } else if (b.kind == BlockKind::Code || b.kind == BlockKind::Citation || b.kind == BlockKind::MathBlock ||
                 b.kind == BlockKind::Table || b.kind == BlockKind::List || b.kind == BlockKind::Abstract)
            add(b.line_start, b.line_end, "");
    }
    std::sort(out.begin(), out.end(), [](const MepmlLspFold &a, const MepmlLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// ===========================================================================
// Formatting and code actions
// ===========================================================================

namespace {

// A table laid out with its pipes lined up, one string per source line
// (the separator row included), plus the lines it replaces.
struct TableLayout {
    int first_line = -1, last_line = -1;
    std::vector<std::string> lines;
};

TableLayout LayOutTable(const Block &b, const std::vector<std::string> &src) {
    TableLayout t;
    size_t cols = 0;
    for (const auto &row : b.rows) cols = std::max(cols, row.size());
    if (cols == 0) return t;
    // Cell text by row, and which source line each row is.
    std::vector<std::vector<std::string>> cells;
    std::vector<int> row_lines;
    for (const auto &row : b.rows) {
        std::vector<std::string> r;
        for (const mepml::TableCell &c : row) r.push_back(Trim(b.text.substr(static_cast<size_t>(c.start), static_cast<size_t>(c.end - c.start))));
        r.resize(cols);
        cells.push_back(r);
        row_lines.push_back(row.empty() ? -1 : At(b, row.front().start).line);
    }
    std::vector<int> width(cols, 3);
    for (const auto &r : cells)
        for (size_t c = 0; c < cols; ++c) width[c] = std::max(width[c], Codepoints(r[c]));
    auto align = [&](size_t c) { return c < b.aligns.size() ? b.aligns[c] : mepml::Align::Default; };
    auto pad = [&](const std::string &s, size_t c) {
        const int room = width[c] - Codepoints(s);
        switch (align(c)) {
            case mepml::Align::Right: return std::string(static_cast<size_t>(room), ' ') + s;
            case mepml::Align::Center: return std::string(static_cast<size_t>(room / 2), ' ') + s + std::string(static_cast<size_t>(room - room / 2), ' ');
            default: return s + std::string(static_cast<size_t>(room), ' ');
        }
    };
    auto row_text = [&](const std::vector<std::string> &r, const std::string &indent) {
        std::string o = indent + "|";
        for (size_t c = 0; c < cols; ++c) o += " " + pad(r[c], c) + " |";
        return o;
    };
    std::string sep = "|";
    for (size_t c = 0; c < cols; ++c) {
        const mepml::Align a = align(c);
        const int w = width[c];
        std::string dash = (a == mepml::Align::Left || a == mepml::Align::Center ? ":" : "") +
                           std::string(static_cast<size_t>(w - ((a == mepml::Align::Left || a == mepml::Align::Right) ? 1 : a == mepml::Align::Center ? 2 : 0)), '-') +
                           (a == mepml::Align::Right || a == mepml::Align::Center ? ":" : "");
        sep += " " + dash + " |";
    }
    // Rows in source order, the separator at its own line.
    std::map<int, std::string> by_line;
    for (size_t r = 0; r < cells.size(); ++r) {
        if (row_lines[r] < 0) return TableLayout{};
        const std::string &orig = src[static_cast<size_t>(row_lines[r])];
        by_line[row_lines[r]] = row_text(cells[r], orig.substr(0, orig.find_first_not_of(" \t")));
    }
    if (b.separator_line >= 0) {
        const std::string &orig = src[static_cast<size_t>(b.separator_line)];
        by_line[b.separator_line] = orig.substr(0, orig.find_first_not_of(" \t")) + sep;
    }
    t.first_line = by_line.begin()->first;
    t.last_line = by_line.rbegin()->first;
    for (int ln = t.first_line; ln <= t.last_line; ++ln) {
        auto it = by_line.find(ln);
        if (it == by_line.end()) return TableLayout{};  // a gap inside the table: leave it alone
        t.lines.push_back(it->second);
    }
    return t;
}

// The edit that replaces a table's lines with its laid-out form, or none.
bool TableEdit(const Block &b, const std::vector<std::string> &lines, MepmlLspTextEdit *edit) {
    const TableLayout t = LayOutTable(b, lines);
    if (t.first_line < 0) return false;
    bool same = true;
    for (int ln = t.first_line; ln <= t.last_line; ++ln) same = same && lines[static_cast<size_t>(ln)] == t.lines[static_cast<size_t>(ln - t.first_line)];
    if (same) return false;
    edit->start_line = t.first_line;
    edit->start_col = 0;
    edit->end_line = t.last_line;
    edit->end_col = Len(lines[static_cast<size_t>(t.last_line)]);
    edit->new_text.clear();
    for (size_t i = 0; i < t.lines.size(); ++i) edit->new_text += (i ? "\n" : "") + t.lines[i];
    return true;
}

}  // namespace

std::vector<MepmlLspTextEdit> MepmlLspFormat(const std::vector<std::string> &lines) {
    const Document &doc = ParsePlain(lines);
    std::vector<MepmlLspTextEdit> edits;
    for (const Block &b : doc.blocks) {
        if (b.kind != BlockKind::Table) continue;
        MepmlLspTextEdit e;
        if (TableEdit(b, lines, &e)) edits.push_back(e);
    }
    return edits;
}

std::vector<MepmlLspCodeAction> MepmlLspCodeActions(const std::vector<std::string> &lines, int line,
                                                    const MepmlLspOptions &opts) {
    std::vector<MepmlLspCodeAction> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const Document &doc = Analyze(lines, opts);
    const std::string &l = lines[static_cast<size_t>(line)];
    auto replace = [&](const std::string &title, const std::string &fixes, int cs, int ce, const std::string &text) {
        MepmlLspCodeAction a;
        a.title = title;
        a.fixes = fixes;
        a.edits.push_back({line, cs, line, ce, text});
        out.push_back(a);
    };

    std::set<std::string> offered_keys;
    for (const MepmlLspDiagnostic &d : MepmlLspDiagnostics(lines, opts)) {
        if (d.line != line) continue;
        const std::string span = l.substr(static_cast<size_t>(std::min(d.col_start, Len(l))),
                                          static_cast<size_t>(std::max(0, std::min(d.col_end, Len(l)) - d.col_start)));
        if (d.code == "unknown-citation") {
            // The key: inside the \cite( parentheses (or braces) of the flagged span.
            const size_t open = span.find_first_of("{("), close = span.find_last_of(")}");
            const std::string key = open != std::string::npos && close != std::string::npos && close > open ? Trim(span.substr(open + 1, close - open - 1)) : "";
            if (key.empty() || !offered_keys.insert(key).second) continue;
            MepmlLspCodeAction a;
            a.title = "Add a \\citation(" + key + ") entry";
            a.fixes = d.code;
            const int last = static_cast<int>(lines.size()) - 1;
            a.edits.push_back({last, Len(lines.back()), last, Len(lines.back()),
                               std::string(Trim(lines.back()).empty() ? "" : "\n") + "\n\\citation(" + key + ",\n  author = {},\n  title = {},\n  year = \n)"});
            out.push_back(a);
            const std::vector<std::string> keys = [&] {
                std::vector<std::string> v;
                for (const auto &kv : doc.citations) v.push_back(kv.first);
                return v;
            }();
            const std::string near = Nearest(key, keys);
            if (!near.empty()) {
                const size_t k = l.find(key, static_cast<size_t>(d.col_start));
                if (k != std::string::npos) replace("Change to \\cite(" + near + ")", d.code, static_cast<int>(k), static_cast<int>(k + key.size()), near);
            }
        } else if (d.code == "unclosed-slide") {
            // Its closing bracket under its last line of content.
            const size_t name = l.find("slide");
            const char close = name != std::string::npos && name + 5 < l.size() && l[name + 5] == '(' ? ')' : '}';
            for (const mepml::Slide &sl : mepml::Slides(doc, static_cast<int>(lines.size()))) {
                if (sl.line_start != line) continue;
                int end = sl.line_end;
                while (end > sl.line_start && Trim(lines[static_cast<size_t>(end)]).empty()) --end;
                MepmlLspCodeAction a;
                a.title = std::string("Close the slide with ") + close;
                a.fixes = d.code;
                a.edits.push_back({end, Len(lines[static_cast<size_t>(end)]), end, Len(lines[static_cast<size_t>(end)]),
                                   std::string("\n") + close});
                out.push_back(a);
            }
        } else if (d.code == "deprecated-directive") {
            const size_t k = l.find("@printbibliography");
            if (k != std::string::npos) replace("Rename to \\bibliography", d.code, static_cast<int>(k), static_cast<int>(k) + 18, "\\bibliography");
            const size_t a = l.find("@abstract{");
            if (a != std::string::npos) {
                // `\abstract(` -> `\abstract(`, and its closing brace, on
                // whichever line it is, -> `)`.
                int depth = 0, cl = -1, cc = -1;
                for (int ln = line; ln < static_cast<int>(lines.size()) && cl < 0; ++ln) {
                    const std::string &t = lines[static_cast<size_t>(ln)];
                    for (size_t k2 = ln == line ? a + 9 : 0; k2 < t.size(); ++k2) {
                        if (t[k2] == '\\') ++k2;
                        else if (t[k2] == '{') ++depth;
                        else if (t[k2] == '}' && --depth == 0) {
                            cl = ln;
                            cc = static_cast<int>(k2);
                            break;
                        }
                    }
                }
                MepmlLspCodeAction fix;
                fix.title = "Change to \\abstract(...)";
                fix.fixes = d.code;
                fix.edits.push_back({line, static_cast<int>(a), line, static_cast<int>(a) + 10, "\\abstract("});
                if (cl >= 0) fix.edits.push_back({cl, cc, cl, cc + 1, ")"});
                out.push_back(fix);
            }
        } else if (d.code == "unknown-directive") {
            const std::string name = span.size() > 1 ? span.substr(1) : "";
            std::vector<std::string> names;
            for (const Vocab &v : DirectiveVocab()) names.push_back(v.name);
            const std::string near = Nearest(name, names);
            if (!near.empty()) replace("Change to @" + near, d.code, d.col_start + 1, d.col_end, near);
        } else if (d.code == "unknown-anchor") {
            std::vector<std::string> slugs;
            for (const Heading &h : Headings(doc, lines)) slugs.push_back("#" + h.slug);
            const std::string near = Nearest(span, slugs);
            if (!near.empty()) replace("Change to " + near, d.code, d.col_start, d.col_end, near);
        } else if (d.code == "unknown-color") {
            const std::string near = Nearest(span, mepml::ColorNames());
            if (!near.empty()) replace("Change to " + near, d.code, d.col_start, d.col_end, near);
        }
    }

    // A table under the cursor: line it up (and pad ragged rows).
    if (const Block *b = OwnBlockAt(doc, line)) {
        MepmlLspTextEdit e;
        if (b->kind == BlockKind::Table && TableEdit(*b, lines, &e)) {
            bool ragged = false;
            size_t cols = 0;
            for (const auto &row : b->rows) cols = std::max(cols, row.size());
            for (const auto &row : b->rows) ragged = ragged || (!row.empty() && row.size() != cols);
            MepmlLspCodeAction a;
            a.title = ragged ? "Pad every row to " + std::to_string(cols) + " cells and align the table" : "Align the table";
            a.kind = ragged ? "quickfix" : "source";
            a.fixes = ragged ? "table-columns" : "";
            a.edits.push_back(e);
            out.push_back(a);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Style sheets (.mepss)

namespace {

bool SheetNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }

struct SheetVocab {
    const char *name;
    const char *doc;
};

const std::vector<SheetVocab> &SheetParts() {
    static const std::vector<SheetVocab> v = {
        {"markup", "An element's syntax characters, on a line shown as its source."},
        {"label", "Generated text naming the element: a box's \"Definition\", a callout's badge, \"Slide 3\", a caption's \"Figure 1\"; a code block's language chip."},
        {"title", "Its title: a box's, the \"Contents\" of a \\toc, a code block's caption in its title bar."},
        {"marker", "A list item's bullet, number or checkbox."},
        {"button", "A code block's title-bar controls: run, stop, fold."},
        {"end", "What closes it: a proof's tombstone."},
        {"key", "The Key of a `//? Key: value` header line."},
        {"value", "The value of a header line."},
        {"name", "An option's name, before its `=`."},
        {"rule", "A table's grid lines."},
        {"header", "The band a code block's, a result's or a slide's title sits in."},
        {"option", "A code block's option chips."},
    };
    return v;
}

const std::vector<std::string> &SheetMediaTags() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out = {"editor", "source", "present", "screen", "print"};
        for (const std::string &t : mepml::KnownFormatTags())
            if (t != "style" && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        return out;
    }();
    return v;
}

const std::vector<std::string> &SheetThemeGroups() {
    static const std::vector<std::string> v = {"Normal", "NormalBg", "Comment", "MutedFg", "Accent", "Border", "Blue", "Cyan", "Green",
                                               "Yellow", "Orange", "Red", "Purple", "OrgHeadlineLevel1", "OrgHeadlineLevel2",
                                               "OrgHeadlineLevel3"};
    return v;
}

// The attributes a selector can test on an element, and (after `=`) the
// values each can have; "" for an attribute that is only present or not.
std::vector<std::pair<std::string, std::vector<std::string>>> SheetAttrs(const std::string &element) {
    std::vector<std::string> box_kinds, callouts;
    for (const mepml::BoxKind &k : mepml::BoxKinds()) box_kinds.push_back(k.name);
    for (const std::string &k : mepml::CalloutKeywords()) callouts.push_back(Lower(k));
    if (element == "heading") return {{"level", {"1", "2", "3", "4", "5", "6"}}};
    if (element == "box") return {{"kind", box_kinds}};
    if (element == "callout") return {{"kind", callouts}};
    if (element == "list-item") return {{"ordered", {}}, {"checked", {"true", "false"}}};
    if (element == "table-cell") return {{"header", {}}};
    if (element == "code") return {{"lang", {}}};
    if (element == "results") return {{"format", {"text", "html", "markdown", "terminal", "gui"}}};
    if (element == "meta") return {{"key", {"title", "subtitle", "author", "date", "option", "import", "style"}}, {"type", {"int", "double", "string"}}};
    if (element == "slide") return {{"number", {}}, {"title", {}}};
    if (element == "column") return {{"width", {}}};
    if (element == "cite") return {{"missing", {}}, {"parenthetical", {}}};
    if (element == "caption") return {{"of", {"figure", "table", "math", "code"}}};
    if (element == "span") return {{"class", {}}};
    if (element == "math") return {{"display", {}}};
    if (element == "document") return {{"type", {"document", "presentation"}}};
    return {};
}

// Where the cursor is in a sheet.
struct SheetContext {
    enum Kind { Selector, Media, Property, Value, None } kind = None;
    std::string property;  // Value: the property being given one
    std::string segment;   // the text of the selector / declaration so far
};

SheetContext SheetContextAt(const std::vector<std::string> &lines, int line, int col) {
    std::string text;
    for (int l = 0; l <= line && l < static_cast<int>(lines.size()); ++l) {
        text += l == line ? lines[static_cast<size_t>(l)].substr(0, static_cast<size_t>(std::clamp(col, 0, Len(lines[static_cast<size_t>(l)])))) : lines[static_cast<size_t>(l)];
        if (l != line) text += '\n';
    }
    SheetContext ctx;
    // The innermost open block: a rule's, an @media's, or none.
    std::vector<bool> open;  // true: a rule's declarations
    size_t seg = 0;          // where the current selector / declaration starts
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            if (end == std::string::npos) return ctx;  // in a comment
            i = end + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < text.size() && text[j] != c && text[j] != '\n') j += text[j] == '\\' ? 2 : 1;
            if (j >= text.size()) return ctx;  // in a string
            i = j;
            continue;
        }
        if (c == '{') {
            const std::string prelude = Trim(text.substr(seg, i - seg));
            open.push_back(prelude.empty() || prelude[0] != '@');
            seg = i + 1;
        } else if (c == '}') {
            if (!open.empty()) open.pop_back();
            seg = i + 1;
        } else if (c == ';') {
            seg = i + 1;
        }
    }
    ctx.segment = text.substr(std::min(seg, text.size()));
    const std::string trimmed = Trim(ctx.segment);
    if (!open.empty() && open.back()) {
        const size_t colon = ctx.segment.find(':');
        if (colon == std::string::npos) {
            ctx.kind = SheetContext::Property;
        } else {
            ctx.kind = SheetContext::Value;
            ctx.property = Lower(Trim(ctx.segment.substr(0, colon)));
        }
    } else if (!trimmed.empty() && trimmed[0] == '@') {
        ctx.kind = SheetContext::Media;
    } else {
        ctx.kind = SheetContext::Selector;
    }
    return ctx;
}

}  // namespace

std::vector<MepmlLspDiagnostic> MepssLspDiagnostics(const std::vector<std::string> &lines) {
    std::string text;
    for (const std::string &l : lines) text += l + "\n";
    const mepml::style::Sheet sheet = mepml::style::Parse(text, "");
    std::vector<MepmlLspDiagnostic> out;
    auto add = [&](int line, int cs, int ce, const std::string &code, const std::string &msg) {
        if (line < 0 || line >= static_cast<int>(lines.size())) return;
        MepmlLspDiagnostic d;
        d.line = line;
        d.col_start = std::clamp(cs, 0, Len(lines[static_cast<size_t>(line)]));
        d.col_end = std::max(d.col_start, std::min(ce, Len(lines[static_cast<size_t>(line)])));
        d.severity = MepmlLspSeverity::Warning;
        d.code = code;
        d.message = msg;
        out.push_back(d);
    };
    for (const mepml::style::Diagnostic &d : sheet.diagnostics) {
        // To the end of the declaration (or of the line).
        const int len = d.line < static_cast<int>(lines.size()) ? Len(lines[static_cast<size_t>(d.line)]) : 0;
        size_t end = d.line < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(d.line)].find_first_of(";{}", static_cast<size_t>(std::min(d.col, len))) : std::string::npos;
        add(d.line, d.col, end == std::string::npos ? len : static_cast<int>(end), "style-error", d.message);
    }
    // Selectors that can select nothing: an element or a part mepml has none of.
    const std::vector<std::string> &elements = mepml::ElementNames();
    for (const mepml::style::Rule &rule : sheet.rules) {
        for (const mepml::style::Selector &sel : rule.selectors) {
            std::string unknown, what;
            for (const mepml::style::Compound &c : sel.compounds)
                if (!c.name.empty() && std::find(elements.begin(), elements.end(), c.name) == elements.end()) unknown = c.name, what = "element";
            if (unknown.empty() && !sel.part.empty() &&
                std::none_of(SheetParts().begin(), SheetParts().end(), [&](const SheetVocab &v) { return sel.part == v.name; }))
                unknown = sel.part, what = "part";
            if (unknown.empty() || rule.line >= static_cast<int>(lines.size())) continue;
            const std::string &l = lines[static_cast<size_t>(rule.line)];
            const size_t at = l.find(unknown);
            const std::vector<std::string> names = what == "element" ? elements : [] {
                std::vector<std::string> p;
                for (const SheetVocab &v : SheetParts()) p.push_back(v.name);
                return p;
            }();
            const std::string near = Nearest(unknown, names);
            add(rule.line, at == std::string::npos ? 0 : static_cast<int>(at), at == std::string::npos ? Len(l) : static_cast<int>(at + unknown.size()),
                "unknown-" + what, "mepml has no " + what + " `" + unknown + "`" + (near.empty() ? "" : " (did you mean `" + near + "`?)"));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const MepmlLspDiagnostic &a, const MepmlLspDiagnostic &b) {
        return a.line != b.line ? a.line < b.line : a.col_start < b.col_start;
    });
    return out;
}

std::vector<MepmlLspCompletionItem> MepssLspCompletions(const std::vector<std::string> &lines, int line, int col) {
    std::vector<MepmlLspCompletionItem> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const std::string &text = lines[static_cast<size_t>(line)];
    col = std::clamp(col, 0, Len(text));
    // The name being typed (hyphens and all), and the part of it mep's
    // client replaces: the letters, digits and underscores at its end.
    int name_start = col;
    while (name_start > 0 && SheetNameChar(text[static_cast<size_t>(name_start - 1)])) --name_start;
    int word_start = col;
    while (word_start > name_start && text[static_cast<size_t>(word_start - 1)] != '-') --word_start;
    const std::string prefix = text.substr(static_cast<size_t>(name_start), static_cast<size_t>(col - name_start));
    const size_t done = static_cast<size_t>(word_start - name_start);
    auto offer = [&](const std::string &label, MepmlLspKind kind, const std::string &detail, const std::string &doc, const std::string &tail = "") {
        if (label.size() < prefix.size() || label.compare(0, prefix.size(), prefix) != 0) return;
        MepmlLspCompletionItem it;
        it.label = label;
        it.insert_text = label.substr(std::min(done, label.size())) + tail;
        it.kind = kind;
        it.detail = detail;
        it.documentation = doc;
        it.replace_start = word_start;
        it.replace_end = col;
        out.push_back(it);
    };
    const char before = name_start > 0 ? text[static_cast<size_t>(name_start - 1)] : '\0';
    const char before2 = name_start > 1 ? text[static_cast<size_t>(name_start - 2)] : '\0';
    const SheetContext ctx = SheetContextAt(lines, line, col);

    if (ctx.kind == SheetContext::Property) {
        for (const mepml::style::PropertyInfo &p : mepml::style::Properties()) offer(p.name, MepmlLspKind::Property, p.values, p.doc, ": ");
        return out;
    }
    if (ctx.kind == SheetContext::Value) {
        // Inside theme( ... : a group of the editor's colour scheme.
        const size_t theme = ctx.segment.rfind("theme(");
        if (theme != std::string::npos && ctx.segment.find_first_of(",)", theme) == std::string::npos) {
            for (const std::string &g : SheetThemeGroups()) offer(g, MepmlLspKind::Color, "theme group", "");
            return out;
        }
        const std::string &p = ctx.property;
        const bool colour = p == "color" || p == "background" || p == "text-decoration-color" || p == "border-color" || p == "border-left-color" ||
                            p.rfind("--", 0) == 0;
        if (colour) {
            offer("theme(", MepmlLspKind::Function, "theme(Group, #fallback)", "The colour the editor's theme gives Group; the fallback wherever there is no theme (an export).");
            offer("fade(", MepmlLspKind::Function, "fade(colour, 0.2)", "The colour at that opacity over what is behind it.");
            offer("var(", MepmlLspKind::Function, "var(--name)", "The value of a custom property.");
            offer("none", MepmlLspKind::Value, "", "");
            for (const std::string &c : mepml::ColorNames()) offer(c, MepmlLspKind::Color, "colour", "");
        } else if (p == "font-weight") {
            for (const char *v : {"normal", "bold"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "font-style") {
            for (const char *v : {"normal", "italic"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "text-decoration") {
            for (const char *v : {"none", "underline", "line-through", "no-underline", "no-line-through"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "font-family") {
            for (const char *v : {"serif", "sans", "mono", "body"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "vertical-align") {
            for (const char *v : {"baseline", "super", "sub"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "text-align") {
            for (const char *v : {"left", "center", "right"}) offer(v, MepmlLspKind::Value, "", "");
        } else if (p == "content") {
            offer("none", MepmlLspKind::Value, "", "The renderer's own text for the part.");
        }
        offer("inherit", MepmlLspKind::Keyword, "", "The parent's value.");
        offer("initial", MepmlLspKind::Keyword, "", "The property's initial value.");
        return out;
    }
    if (ctx.kind == SheetContext::Media) {
        if (before == '@') offer("media", MepmlLspKind::Keyword, "@media tags { rules }", "Rules for some renderers only.", " ");
        else
            for (const std::string &t : SheetMediaTags()) offer(t, MepmlLspKind::Value, "medium", "");
        return out;
    }
    if (ctx.kind != SheetContext::Selector) return out;
    if (before == '@') {
        offer("media", MepmlLspKind::Keyword, "@media tags { rules }", "Rules for some renderers only.", " ");
    } else if (before == ':' && before2 == ':') {
        for (const SheetVocab &v : SheetParts()) offer(v.name, MepmlLspKind::Field, "part", v.doc);
    } else if (before == ':') {
        offer("active", MepmlLspKind::Keyword, "state", "The block the cursor is in.");
    } else if (before == '[' || before == '=') {
        // The element the bracket belongs to: the name right before it.
        int open = name_start - 1;
        while (open >= 0 && text[static_cast<size_t>(open)] != '[') --open;
        int e = open;
        while (e > 0 && SheetNameChar(text[static_cast<size_t>(e - 1)])) --e;
        const std::string element = open >= 0 ? text.substr(static_cast<size_t>(e), static_cast<size_t>(open - e)) : std::string();
        const auto attrs = SheetAttrs(element);
        if (before == '[') {
            for (const auto &a : attrs) offer(a.first, MepmlLspKind::Property, "attribute of " + element, "", a.second.empty() ? "" : "=");
        } else {
            const std::string attr = Trim(text.substr(static_cast<size_t>(open + 1), static_cast<size_t>(name_start - 1 - (open + 1))));
            for (const auto &a : attrs)
                if (a.first == attr)
                    for (const std::string &v : a.second) offer(v, MepmlLspKind::Value, attr, "");
        }
    } else {
        for (const std::string &name : mepml::ElementNames()) offer(name, MepmlLspKind::Module, "element", "");
    }
    return out;
}

MepmlLspHoverInfo MepssLspHover(const std::vector<std::string> &lines, int line, int col) {
    MepmlLspHoverInfo info;
    if (line < 0 || line >= static_cast<int>(lines.size())) return info;
    const std::string &text = lines[static_cast<size_t>(line)];
    col = std::clamp(col, 0, Len(text));
    int a = col, b = col;
    while (a > 0 && SheetNameChar(text[static_cast<size_t>(a - 1)])) --a;
    while (b < Len(text) && SheetNameChar(text[static_cast<size_t>(b)])) ++b;
    if (b <= a) return info;
    const std::string word = text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
    auto found = [&](const std::string &what) {
        info.found = true;
        info.text = what;
        info.line = line;
        info.col_start = a;
        info.col_end = b;
        return info;
    };
    const bool part = a >= 2 && text[static_cast<size_t>(a - 1)] == ':' && text[static_cast<size_t>(a - 2)] == ':';
    if (part) {
        for (const SheetVocab &v : SheetParts())
            if (word == v.name) return found("::" + word + " -- " + v.doc);
        return info;
    }
    const bool call = b < Len(text) && text[static_cast<size_t>(b)] == '(';
    if (call && word == "theme") return found("theme(Group, #fallback): the colour the editor's theme gives Group, and the fallback wherever there is no theme (every export).");
    if (call && word == "fade") return found("fade(colour, alpha): the colour at that opacity (0-1) over what is behind it.");
    if (call && word == "var") return found("var(--name, fallback): the value of a custom property; custom properties are inherited.");
    const SheetContext ctx = SheetContextAt(lines, line, b);
    if (ctx.kind == SheetContext::Property || (ctx.kind == SheetContext::Value && ctx.property == Lower(word)))
        for (const mepml::style::PropertyInfo &p : mepml::style::Properties())
            if (Lower(word) == p.name) return found(std::string(p.name) + ": " + p.values + "\n" + p.doc + (p.inherited ? "\nInherited." : ""));
    if (ctx.kind == SheetContext::Selector) {
        const std::vector<std::string> &elements = mepml::ElementNames();
        if (std::find(elements.begin(), elements.end(), word) != elements.end()) {
            std::string attrs;
            for (const auto &at : SheetAttrs(word)) attrs += (attrs.empty() ? "" : ", ") + at.first;
            return found("The mepml element `" + word + "`" + (attrs.empty() ? "." : "; attributes: " + attrs + ".") + "\n(docs/mepml-spec/structure.md)");
        }
    }
    return info;
}

