#include "a11y_doc.h"

#include <algorithm>
#include <cctype>

namespace a11y {

namespace {

bool IsBlank(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

// Runs of blanks as one space, none at either end.
std::string Squeeze(const std::string &s) {
    std::string out;
    bool gap = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            gap = !out.empty();
        } else {
            if (gap) out += ' ';
            gap = false;
            out += c;
        }
    }
    return out;
}

std::string Cut(const std::string &s, int cols) {
    if (cols <= 0 || static_cast<int>(s.size()) <= cols) return s;
    size_t n = static_cast<size_t>(cols);
    // (Not inside a UTF-8 sequence.)
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "...";
}

// Appends a piece of a line: glyph runs (a PDF's) take a space between
// them unless the next starts with closing punctuation.
void Join(const Document &doc, std::string *line, const std::string &piece) {
    if (piece.empty()) return;
    if (!doc.spaced && !line->empty() && !std::isspace(static_cast<unsigned char>(line->back())) &&
        !std::isspace(static_cast<unsigned char>(piece.front()))) {
        static const std::string kClosing = ".,;:!?)]}";
        static const std::string kOpening = "([{";
        if (kClosing.find(piece.front()) == std::string::npos && kOpening.find(line->back()) == std::string::npos) *line += ' ';
    }
    *line += piece;
}

void Inline(const Document &doc, const Node &n, std::string *line) {
    switch (n.role) {
        case Role::Text: Join(doc, line, n.text); return;
        case Role::Figure:
            if (n.decorative) return;
            Join(doc, line, n.has_alt && !n.alt.empty() ? "[figure: " + Squeeze(n.alt) + "]" : "[figure]");
            return;
        case Role::Formula: {
            if (n.has_alt && !n.alt.empty()) {
                Join(doc, line, Squeeze(n.alt));
                return;
            }
            break;
        }
        case Role::Label: {
            std::string label;
            for (const Node &c : n.children) Inline(doc, c, &label);
            if (!IsBlank(label)) Join(doc, line, Squeeze(label) + " ");
            return;
        }
        default: break;
    }
    if (n.has_alt && !n.alt.empty() && n.role != Role::Table) {
        Join(doc, line, Squeeze(n.alt));
        return;
    }
    // (A caption is a line of its own under what it captions.)
    for (const Node &c : n.children)
        if (c.role != Role::Caption) Inline(doc, c, line);
}

bool IsBlockRole(Role r) {
    switch (r) {
        case Role::Text:
        case Role::Link:
        case Role::Label:
        case Role::Group: return false;
        default: return true;
    }
}

// Whether a node holds blocks (it is then walked) rather than a line of text.
bool HoldsBlocks(const Node &n) {
    for (const Node &c : n.children) {
        if (c.role == Role::Formula || c.role == Role::Figure) continue;  // inline in a line of text
        if (IsBlockRole(c.role)) return true;
        if (c.role == Role::Group && HoldsBlocks(c)) return true;
    }
    return false;
}

struct Reader {
    const Document &doc;
    std::vector<Line> out;

    void Emit(const Node &n, int depth, const std::string &text) {
        Line l;
        l.text = text;
        l.depth = depth;
        l.page = n.page;
        l.line = n.line;
        out.push_back(std::move(l));
    }
    std::string TextOf(const Node &n) { return Squeeze(InlineText(doc, n)); }
    // The text of a node's children that are not blocks of their own.
    std::string OwnText(const Node &n) {
        std::string line;
        for (const Node &c : n.children)
            if (!IsBlockRole(c.role) || c.role == Role::Formula || c.role == Role::Figure) Inline(doc, c, &line);
        return Squeeze(line);
    }

    void Cells(const Node &n, std::vector<const Node *> *cells) {
        for (const Node &c : n.children) {
            if (c.role == Role::Cell || c.role == Role::HeaderCell) cells->push_back(&c);
            else if (c.role == Role::Group) Cells(c, cells);
        }
    }
    void Rows(const Node &n, std::vector<const Node *> *rows) {
        for (const Node &c : n.children) {
            if (c.role == Role::Row) rows->push_back(&c);
            else if (c.role == Role::Group) Rows(c, rows);
        }
    }
    void Table(const Node &n, int depth) {
        std::vector<const Node *> rows;
        Rows(n, &rows);
        size_t cols = 0;
        std::vector<std::vector<const Node *>> grid;
        for (const Node *r : rows) {
            grid.emplace_back();
            Cells(*r, &grid.back());
            cols = std::max(cols, grid.back().size());
        }
        std::string head = "Table: " + std::to_string(rows.size()) + (rows.size() == 1 ? " row, " : " rows, ") + std::to_string(cols) +
                           (cols == 1 ? " column." : " columns.");
        if (!n.summary.empty()) head += " " + Squeeze(n.summary);
        Emit(n, depth, head);
        for (const Node &c : n.children)
            if (c.role == Role::Caption) Emit(c, depth + 1, "Caption: " + TextOf(c));
        // The column headers: the first row, when every cell of it is one.
        std::vector<std::string> headers;
        size_t first = 0;
        if (!grid.empty() && !grid[0].empty() &&
            std::all_of(grid[0].begin(), grid[0].end(), [](const Node *c) { return c->role == Role::HeaderCell; })) {
            for (const Node *c : grid[0]) headers.push_back(TextOf(*c));
            std::string line = "Columns: ";
            for (size_t i = 0; i < headers.size(); ++i) line += (i ? ", " : "") + (headers[i].empty() ? std::string("(empty)") : headers[i]);
            Emit(*rows[0], depth + 1, line);
            first = 1;
        }
        for (size_t r = first; r < grid.size(); ++r) {
            std::string line = "Row " + std::to_string(r - first + 1) + ": ";
            for (size_t c = 0; c < grid[r].size(); ++c) {
                std::string cell = TextOf(*grid[r][c]);
                if (cell.empty()) cell = "(empty)";
                if (c) line += "; ";
                if (c < headers.size() && !headers[c].empty()) line += headers[c] + ": ";
                line += cell;
            }
            Emit(*rows[r], depth + 1, line);
        }
    }
    int CountItems(const Node &n) {
        int count = 0;
        for (const Node &c : n.children) {
            if (c.role == Role::ListItem) ++count;
            else if (c.role == Role::Group) count += CountItems(c);
        }
        return count;
    }
    void List(const Node &n, int depth) {
        const int count = CountItems(n);
        Emit(n, depth, "List of " + std::to_string(count) + (count == 1 ? " item" : " items"));
        Items(n, depth + 1);
    }
    void Items(const Node &n, int depth) {
        for (const Node &c : n.children) {
            if (c.role == Role::Group) {
                Items(c, depth);
            } else if (c.role == Role::ListItem) {
                Item(c, depth);
            } else {
                Walk(c, depth);
            }
        }
    }
    // An item's label and the text straight in it on one line, then the
    // blocks it holds (a list inside it, a paragraph).
    void ItemParts(const Node &n, std::string *line, std::vector<const Node *> *blocks) {
        for (const Node &c : n.children) {
            if (c.role == Role::Group) {
                ItemParts(c, line, blocks);
            } else if (c.role == Role::Paragraph && blocks->empty() && IsBlank(*line)) {
                Inline(doc, c, line);  // (an item whose text is a paragraph)
            } else if (IsBlockRole(c.role) && c.role != Role::Formula && c.role != Role::Figure) {
                blocks->push_back(&c);
            } else {
                Inline(doc, c, line);
            }
        }
    }
    void Item(const Node &n, int depth) {
        std::string line;
        std::vector<const Node *> blocks;
        std::string label;
        for (const Node &c : n.children)
            if (c.role == Role::Label) Inline(doc, c, &label);
        Node rest = n;
        rest.children.erase(std::remove_if(rest.children.begin(), rest.children.end(), [](const Node &c) { return c.role == Role::Label; }),
                            rest.children.end());
        ItemParts(rest, &line, &blocks);
        label = Squeeze(label);
        Emit(n, depth, (label.empty() ? std::string("-") : label) + " " + Squeeze(line));
        for (const Node *b : blocks) Walk(*b, depth + 1);
    }

    void Walk(const Node &n, int depth) {
        switch (n.role) {
            case Role::Text:
            case Role::Link:
            case Role::Label: {
                const std::string t = TextOf(n);
                if (!t.empty()) Emit(n, depth, t);
                return;
            }
            case Role::Title: Emit(n, depth, "Title: " + TextOf(n)); return;
            case Role::Heading: Emit(n, depth, "Heading level " + std::to_string(std::max(1, n.level)) + ": " + TextOf(n)); return;
            case Role::Figure:
                if (n.decorative) return;
                Emit(n, depth, n.has_alt && !n.alt.empty() ? "Figure: " + Squeeze(n.alt) : "Figure with no description.");
                for (const Node &c : n.children)
                    if (c.role == Role::Caption) Walk(c, depth + 1);
                return;
            case Role::Formula: {
                const std::string t = TextOf(n);
                Emit(n, depth, t.empty() ? "Formula with no description." : "Formula: " + t);
                for (const Node &c : n.children)
                    if (c.role == Role::Caption) Walk(c, depth + 1);
                return;
            }
            case Role::Caption: Emit(n, depth, "Caption: " + TextOf(n)); return;
            case Role::Table: Table(n, depth); return;
            case Role::List: List(n, depth); return;
            case Role::ListItem: Item(n, depth); return;
            case Role::Code: {
                // Code is read as written: line by line.
                std::string text;
                for (const Node &c : n.children)
                    if (c.role == Role::Text) text += c.text + (doc.spaced ? "" : "\n");
                Emit(n, depth, "Code:");
                size_t at = 0;
                while (at <= text.size()) {
                    size_t nl = text.find('\n', at);
                    if (nl == std::string::npos) nl = text.size();
                    const std::string l = text.substr(at, nl - at);
                    if (!(nl == text.size() && IsBlank(l))) Emit(n, depth + 1, l);
                    at = nl + 1;
                }
                for (const Node &c : n.children)
                    if (c.role != Role::Text) Walk(c, depth + 1);
                return;
            }
            default: break;
        }
        // A container. One that holds only a line of text reads as that
        // line; otherwise its own text first, then its blocks.
        std::string lead;
        switch (n.role) {
            case Role::Quote: lead = "Quote: "; break;
            case Role::Note: lead = "Note: "; break;
            default: break;
        }
        if (!HoldsBlocks(n)) {
            const std::string t = TextOf(n);
            if (!t.empty()) Emit(n, depth, lead + t);
            return;
        }
        int inner = depth;
        if (n.role == Role::Quote || n.role == Role::Note) {
            Emit(n, depth, lead.substr(0, lead.size() - 2) + ":");
            inner = depth + 1;
        }
        // Runs of inline children between the blocks read as lines of their own.
        std::string run;
        const Node *run_first = nullptr;
        auto flush = [&] {
            const std::string t = Squeeze(run);
            if (!t.empty() && run_first) Emit(*run_first, inner, t);
            run.clear();
            run_first = nullptr;
        };
        for (const Node &c : n.children) {
            if (IsBlockRole(c.role) || (c.role == Role::Group && HoldsBlocks(c))) {
                flush();
                Walk(c, inner);
            } else {
                if (!run_first) run_first = &c;
                Inline(doc, c, &run);
            }
        }
        flush();
    }
};

void OutlineWalk(const Document &doc, const Node &n, int depth, int cols, std::vector<Line> *out) {
    Line l;
    l.depth = depth;
    l.page = n.page;
    l.line = n.line;
    if (n.role == Role::Text) {
        const std::string t = Squeeze(n.text);
        if (t.empty()) return;
        l.text = "\"" + Cut(t, cols) + "\"";
        out->push_back(std::move(l));
        return;
    }
    l.text = RoleName(n.role);
    if (n.role == Role::Heading) l.text += " " + std::to_string(n.level);
    if (!n.tag.empty() && n.tag != l.text) l.text += " <" + n.tag + ">";
    if (n.decorative) l.text += " (decorative)";
    else if (n.has_alt) l.text += " alt=\"" + Cut(Squeeze(n.alt), cols) + "\"";
    if (!n.summary.empty()) l.text += " summary=\"" + Cut(Squeeze(n.summary), cols) + "\"";
    if (!n.scope.empty()) l.text += " scope=" + n.scope;
    if (!n.lang.empty()) l.text += " lang=" + n.lang;
    out->push_back(std::move(l));
    for (const Node &c : n.children) OutlineWalk(doc, c, depth + 1, cols, out);
}

struct Checker {
    const Document &doc;
    std::vector<Issue> out;
    int last_heading = 0;
    int headings = 0;
    int blocks = 0;
    int formulas_without_alt = 0;
    size_t first_formula = 0;  // index into `out` of the note about them

    void Add(Severity s, const char *code, const std::string &message, const Node &n) {
        Issue i;
        i.severity = s;
        i.code = code;
        i.message = message;
        i.page = n.page;
        i.line = n.line;
        out.push_back(std::move(i));
    }
    static bool HasRole(const Node &n, Role r) {
        if (n.role == r) return true;
        for (const Node &c : n.children)
            if (HasRole(c, r)) return true;
        return false;
    }
    void Walk(const Node &n) {
        if (IsBlockRole(n.role)) ++blocks;
        switch (n.role) {
            case Role::Figure:
                if (!n.decorative && (!n.has_alt || IsBlank(n.alt)))
                    Add(Severity::Error, "figure-alt",
                        "Figure has no alternative text: a reader who cannot see it is told nothing about it", n);
                break;
            case Role::Formula:
                // (One note for the document, at the first: a paper has dozens.)
                if (!n.has_alt || IsBlank(n.alt)) {
                    if (formulas_without_alt++ == 0) first_formula = out.size();
                    if (formulas_without_alt == 1)
                        Add(Severity::Info, "formula-alt", "Formula has no alternative text: it is read from its symbols or its source", n);
                }
                break;
            case Role::Table: {
                if (!HasRole(n, Role::HeaderCell))
                    Add(Severity::Warning, "table-header", "Table has no header cells: its values are read without what they are values of", n);
                bool caption = false;
                for (const Node &c : n.children) caption = caption || c.role == Role::Caption;
                if (n.summary.empty() && !caption)
                    Add(Severity::Info, "table-summary", "Table has neither a caption nor a description of what it shows", n);
                break;
            }
            case Role::Heading: {
                ++headings;
                if (IsBlank(InlineText(doc, n))) Add(Severity::Warning, "heading-empty", "Heading has no text", n);
                if (last_heading > 0 && n.level > last_heading + 1)
                    Add(Severity::Warning, "heading-skip",
                        "Heading level " + std::to_string(n.level) + " follows level " + std::to_string(last_heading) +
                            ": a level is skipped",
                        n);
                last_heading = n.level;
                break;
            }
            case Role::Link:
                if (IsBlank(InlineText(doc, n))) Add(Severity::Warning, "link-text", "Link has no text to be read as", n);
                break;
            default: break;
        }
        for (const Node &c : n.children) Walk(c);
    }
};

}  // namespace

const char *RoleName(Role role) {
    switch (role) {
        case Role::Document: return "Document";
        case Role::Section: return "Section";
        case Role::Title: return "Title";
        case Role::Heading: return "Heading";
        case Role::Paragraph: return "Paragraph";
        case Role::Text: return "Text";
        case Role::List: return "List";
        case Role::ListItem: return "ListItem";
        case Role::Label: return "Label";
        case Role::Table: return "Table";
        case Role::Row: return "Row";
        case Role::HeaderCell: return "HeaderCell";
        case Role::Cell: return "Cell";
        case Role::Figure: return "Figure";
        case Role::Formula: return "Formula";
        case Role::Caption: return "Caption";
        case Role::Code: return "Code";
        case Role::Quote: return "Quote";
        case Role::Note: return "Note";
        case Role::Link: return "Link";
        case Role::Group: return "Group";
    }
    return "Group";
}

const char *SeverityName(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
    }
    return "info";
}

std::string InlineText(const Document &doc, const Node &node) {
    std::string line;
    Inline(doc, node, &line);
    return line;
}

std::vector<Line> Read(const Document &doc) {
    Reader r{doc, {}};
    Node head;
    r.Emit(head, 0, doc.title.empty() ? "Document with no title." : "Document: " + Squeeze(doc.title));
    if (!doc.lang.empty()) r.Emit(head, 0, "Language: " + doc.lang);
    if (!doc.tagged) r.Emit(head, 0, "No structure to read: the text follows in page order.");
    for (const Node &c : doc.root.children) r.Walk(c, 0);
    return std::move(r.out);
}

std::vector<Line> Outline(const Document &doc, int text_cols) {
    std::vector<Line> out;
    Line head;
    head.text = "Document";
    if (!doc.title.empty()) head.text += " title=\"" + Cut(Squeeze(doc.title), text_cols) + "\"";
    if (!doc.lang.empty()) head.text += " lang=" + doc.lang;
    if (!doc.tagged) head.text += " (no structure)";
    out.push_back(std::move(head));
    for (const Node &c : doc.root.children) OutlineWalk(doc, c, 1, text_cols, &out);
    return out;
}

std::vector<Issue> Check(const Document &doc) {
    Checker c{doc, {}};
    Node head;
    if (!doc.tagged)
        c.Add(Severity::Error, "doc-untagged",
              "The document has no structure (it is not tagged): a screen reader gets only its text, in page order", head);
    if (IsBlank(doc.title)) c.Add(Severity::Warning, "doc-title", "The document has no title", head);
    if (IsBlank(doc.lang))
        c.Add(Severity::Warning, "doc-lang", "The document does not say what language it is in: a screen reader has to guess its voice",
              head);
    for (const Node &n : doc.root.children) c.Walk(n);
    if (c.formulas_without_alt > 1)
        c.out[c.first_formula].message = std::to_string(c.formulas_without_alt) +
                                         " formulas have no alternative text: they are read from their symbols or their source (the first is here)";
    if (doc.tagged && c.headings == 0 && c.blocks > 12)
        c.Add(Severity::Info, "doc-headings", "The document has no headings to move through it by", head);
    return std::move(c.out);
}

std::string Report(const Document &doc, const std::string &what) {
    std::string out;
    if (what == "check") {
        const std::vector<Issue> issues = Check(doc);
        for (const Issue &i : issues) {
            out += std::string(SeverityName(i.severity)) + ": " + i.message;
            if (i.page >= 0) out += " (page " + std::to_string(i.page + 1) + ")";
            if (i.line >= 0) out += " (line " + std::to_string(i.line + 1) + ")";
            out += " [" + i.code + "]\n";
        }
        if (issues.empty()) out = "No accessibility problems found.\n";
        return out;
    }
    for (const Line &l : what == "tree" ? Outline(doc) : Read(doc)) out += std::string(static_cast<size_t>(l.depth) * 2, ' ') + l.text + "\n";
    return out;
}

}  // namespace a11y
