#ifndef MEP_MEPML_DOC_H
#define MEP_MEPML_DOC_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// mepml: mep's own lightweight markup language for literate-programming
// style documents (prose + math + executable code blocks + their results).
// The reference for everything the language can express is the repo's own
// test.mepml; help/mepml.org documents it for users.
//
// Deliberately raylib-free, same reasoning as org_doc.h: the parser, the
// per-line highlight spans the editor draws from, and the HTML exporter are
// pure functions over plain std::string lines, so all of it is testable
// without a GL context (mepml_doc_test.cpp). editor.cpp owns turning the
// spans into Decorations; main.cpp's kBuiltinMepml owns keymaps, running
// code blocks and export.
//
// Positions follow the Decoration convention used everywhere else: 0-based
// line indices into the buffer's `lines`, byte columns, half-open ranges.

namespace mepml {

// ---------------------------------------------------------------------------
// Values: `//? Option: Name=value` and ```{lang, Name=value} code headers.
// A value is an int if it parses entirely as one, else a double if it
// parses entirely as one, else a string ("quoted" strings lose their quotes
// and understand \" and \\ escapes).
enum class ValueKind { Int, Double, String };

struct Value {
    ValueKind kind = ValueKind::String;
    long long i = 0;
    double d = 0.0;
    std::string s;  // always holds the (unquoted) text, whatever the kind

    static Value Parse(const std::string &raw);
};

struct Option {
    std::string name;
    Value value;
    int line = -1;
};

// ---------------------------------------------------------------------------
// Inline content.

enum class InlineKind {
    Text,
    Bold,         // *x*
    Italic,       // ~x~
    Underline,    // _x_
    Superscript,  // ^x^
    Subscript,    // ,,x,,
    Small,        // <x>
    Big,          // >x<
    Mono,         // |x|        monospaced, still formatted inside
    Highlight,    // =x=
    Strike,       // -x-
    Insert,       // +x+
    Delete,       // !x!
    Verbatim,     // `x`        literal, no formatting inside
    Link,         // [text|url] or [url]; `arg` is the url
    Font,         // \f{family}{x};  `arg` is the family
    FontSize,     // \fs{pt}{x};     `arg` is the size in points
    Color,        // \color{c}{x};   `arg` is a name or #rrggbb
    Footnote,     // \fn{x}; `number` is its 1-based document order
    Cite,         // \cite{key}   textual: Author (Year)
    CiteP,        // \citep{key}  parenthetical: (Author, Year)
    Math,         // $x$ or \(x\); `text` is the TeX, `alt` any @alttext{};
                  // `arg` is "display" for an inline $$x$$
    Comment,      // `text // comment` to the end of the line (not rendered)
};

struct Inline {
    InlineKind kind = InlineKind::Text;
    std::string text;  // Text/Verbatim/Math contents, Cite key
    std::string arg;   // Link url, Font family, FontSize size, Color colour
    std::string alt;   // Math @alttext{}
    int number = 0;    // Footnote number
    std::vector<Inline> children;
    // Source range of the whole construct including its markup, as byte
    // offsets into the owning block's joined text (see Block::text).
    int start = 0, end = 0;
    // Range of the construct's *content* (children / text) within it.
    int inner_start = 0, inner_end = 0;
};

// ---------------------------------------------------------------------------
// Blocks.

enum class BlockKind {
    Paragraph,
    Heading,       // > / >> / >>> ... (up to 6)
    Comment,       // // text  (not rendered in output)
    Callout,       // // NOTE: text (and following // lines)
    Meta,          // //? Key: value (document metadata)
    Import,        // @import{path}
    Citation,      // @citation{key}{ field = value, ... }
    MathBlock,     // $$ ... $$  or  \[ ... \]
    Code,          // ```{lang, opts} ... ```
    Image,         // @image{path}
    Table,         // | a | b |
    List,          // - item / 1. item / - [ ] task
    Rule,          // --- (3+ of - = _ *)
    Bibliography,  // @bibliography (the older @printbibliography still parses)
    TableOfContents,  // @toc
    Abstract,      // @abstract{ prose, blank lines between paragraphs }
};

enum class Align { Default, Left, Center, Right };

struct ListItem {
    int line = -1;          // first line of the item
    int indent = 0;         // columns of leading whitespace
    bool ordered = false;
    int number = 0;         // for ordered items
    int checkbox = -1;      // -1 none, 0 "[ ]", 1 "[x]"
    int content_start = 0;  // offset into Block::text of the item body
    int content_end = 0;
    std::vector<Inline> content;
};

struct TableCell {
    std::vector<Inline> content;
    int start = 0, end = 0;  // offsets into Block::text
    // A cell holding nothing but `@image{path}` is a picture: the path,
    // "" for an ordinary cell. `content` still has the raw text.
    std::string image;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int line_start = -1, line_end = -1;  // inclusive
    // Non-empty for a block that came from an @import: the imported file's
    // path. Line numbers are then that file's, not the buffer's.
    std::string origin;

    // The block's source text, lines joined with '\n' (exactly the buffer
    // bytes from line_start's column 0 to line_end's end). Inline offsets
    // index this; OffsetToPos maps them back to (line, col).
    std::string text;
    std::vector<Inline> inlines;  // Paragraph, Heading title, Callout body, captions

    int level = 0;               // Heading
    std::string keyword;         // Callout: NOTE/WARNING/...; Meta: key
    std::string value;           // Meta raw value; Import/Image path; Citation key
    std::vector<Option> options;  // Code (preceding //? lines + header); Meta Option
    std::string lang;            // Code
    std::string code;            // Code body (between fences); MathBlock TeX
    int code_line_start = -1, code_line_end = -1;  // body lines, inclusive; empty => start > end
    // Code results: the `// result_begin:` ... `// result_end` region right
    // after the closing fence, if present (line indices, inclusive, markers
    // included), plus the result text with the `// ` prefixes stripped.
    int result_line_start = -1, result_line_end = -1;
    std::vector<std::string> result_lines;
    // Result lines that are a figure the block produced (`// @image{path}`,
    // written by a run with a `file=` option): line index and path.
    std::vector<std::pair<int, std::string>> result_images;
    // What the results are: "" for plain text output, "html" for HTML the
    // block produced (a `results=html` option, recorded on the results'
    // own opening line as `// result_begin: html`). The editor renders an
    // html result in place and the exports embed it. "markdown"
    // (`results=markdown`) is Markdown the block printed, written between
    // the markers without `// ` and parsed as blocks of the document: the
    // block owns only the markers (line_end is the opening one), and
    // result_lines holds the raw lines, which every export skips.
    std::string result_format;

    std::map<std::string, std::string> fields;  // Citation fields, lowercase names
    std::vector<std::string> field_order;

    // Caption / alt text attached by following @caption{} / @alttext{}
    // lines (Image, Table, MathBlock).
    std::string caption, alt;
    std::vector<Inline> caption_inlines;
    int caption_line = -1, alt_line = -1;

    // Table
    std::vector<std::vector<TableCell>> rows;
    std::vector<Align> aligns;
    int header_rows = 0;  // rows above the |---| separator (0 = no header)
    int separator_line = -1;
    int rows_end = -1;    // the table's last row (before any caption or marker)

    std::vector<ListItem> items;  // List

    // Abstract: `inlines` holds all of its prose; paragraph k is
    // inlines[paragraph_starts[k] .. paragraph_starts[k+1]) (see
    // AbstractParagraphs). Always starts with 0 when there is any text.
    std::vector<size_t> paragraph_starts;

    // Offset (into text) of the first byte that belongs to each line.
    std::vector<int> line_offsets;
    struct Pos {
        int line = 0, col = 0;
    };
    Pos OffsetToPos(int offset) const;
};

struct Diagnostic {
    enum Severity { Error, Warning, Info };
    Severity severity = Warning;
    int line = 0, col_start = 0, col_end = 0;
    std::string message;
};

struct Citation {
    std::string key;
    std::map<std::string, std::string> fields;
    std::vector<std::string> field_order;
    int line = -1;
};

struct Document {
    std::string title;
    std::vector<std::pair<std::string, std::string>> meta;  // every //? Key: value, in order
    std::vector<Option> options;                             // document-level //? Option:
    std::vector<Block> blocks;
    std::map<std::string, Citation> citations;
    std::vector<std::string> cite_order;  // keys in first-cited order
    int footnote_count = 0;
    std::vector<Diagnostic> diagnostics;

    const Option *FindOption(const std::string &name) const;
    // Index into blocks of the block covering `line`, or -1.
    int BlockAtLine(int line) const;
};

// An @abstract block's paragraphs, each its own run of inlines.
std::vector<std::vector<Inline>> AbstractParagraphs(const Block &b);

// The callout keywords a `// KEYWORD:` comment recognises.
const std::vector<std::string> &CalloutKeywords();

Document Parse(const std::vector<std::string> &lines);
// A line's heading depth by its own text (`>`..`>>>>>>` then a space), 0
// otherwise. Context-free: Document::blocks is the authority on whether the
// line really is a heading (it is not inside a code block, say).
int LineHeadingLevel(const std::string &line);
// Bytes of heading markup at the start of a heading line (the `>`s and the
// blanks after them), 0 for any other line.
int LineHeadingMarkupLen(const std::string &line);
// Parse inline markup in `text` (used for single-line fragments such as
// captions). Footnotes are numbered from *footnote_counter if given.
std::vector<Inline> ParseInlines(const std::string &text, int *footnote_counter = nullptr);

// ---------------------------------------------------------------------------
// @import resolution. `read` returns false when the file cannot be read.
// Imports are expanded in place (their blocks' line numbers stay those of
// the imported file; `origin` in the result names which file). Cycles and
// missing files become diagnostics on the @import line.
using ReadFileFn = std::function<bool(const std::string &path, std::vector<std::string> *lines)>;
std::string ResolvePath(const std::string &base_file, const std::string &path);
Document ParseWithImports(const std::string &file, const std::vector<std::string> &lines, const ReadFileFn &read);

// ---------------------------------------------------------------------------
// Editor highlighting. One span per styled run on one line; a construct
// that crosses lines produces one span per line. `markup` spans are the
// syntax characters (`*`, `\color{red}{`, `}` ...) that the editor may
// conceal; `replace` is text to draw in their place when concealed (a
// footnote's number, a citation's rendered label, a callout's badge).

enum StyleFlag : std::uint32_t {
    kBold = 1u << 0,
    kItalic = 1u << 1,
    kUnderline = 1u << 2,
    kStrike = 1u << 3,
    kSuper = 1u << 4,
    kSub = 1u << 5,
    kSmall = 1u << 6,
    kBig = 1u << 7,
    kMono = 1u << 8,
    kHighlight = 1u << 9,
    kInsert = 1u << 10,
    kDelete = 1u << 11,
    kVerbatim = 1u << 12,
    kLink = 1u << 13,
    kMath = 1u << 14,
    kComment = 1u << 15,
    kMeta = 1u << 16,
    kDirective = 1u << 17,  // @image{...} etc.
    kCode = 1u << 18,       // code block body
    kResult = 1u << 19,     // code result region
    kTable = 1u << 20,
    kCite = 1u << 21,
    kFootnote = 1u << 22,
    kCallout = 1u << 23,
    kRule = 1u << 24,
    kListMarker = 1u << 25,
    kHeading = 1u << 26,
    kError = 1u << 27,  // unresolved reference etc.
    kTableRule = 1u << 28,  // a table's own `|` pipes / |---| separator row
    kAbstract = 1u << 29,   // an @abstract's `@abstract{` / `}` (replace = its "Abstract" label)
};

struct Span {
    int line = 0, col_start = 0, col_end = 0;
    std::uint32_t style = 0;
    bool markup = false;
    std::string replace;       // conceal replacement (markup spans only)
    int heading_level = 0;     // kHeading
    std::string color;         // \color{...} value
    std::string font;          // \f{...} family
    float font_size = 0.0f;    // \fs{...} points, 0 = unset
    std::string callout;       // kCallout keyword
    std::string target;        // kLink url, kCite key, import/image path
};

std::vector<Span> Highlight(const Document &doc);

// A results line (its `// ` prefix already stripped) that names a figure:
// `@image{path}`. Sets *path.
bool ResultImagePath(const std::string &text, std::string *path);

// "Figure N" / "Table N" for every numbered block, "" for the rest,
// parallel to doc.blocks. Images and code blocks that produced a figure
// share one sequence; tables have their own. The editor's captions and
// the HTML export both number from this, so they always agree. What the
// exports leave out (see CodeExports) is not numbered.
std::vector<std::string> BlockLabels(const Document &doc);

// --- Generated content: the table of contents and the bibliography, as
// lines of styled text (the editor draws these in place of the @toc /
// @bibliography row; the HTML export builds the same content as markup).

struct RenderedSpan {
    int col_start = 0, col_end = 0;  // byte offsets into RenderedLine::text
    std::uint32_t style = 0;         // kBold / kItalic / kHeading / kComment / kCite / kLink
    int heading_level = 0;           // with kHeading
};
struct RenderedLine {
    std::string text;
    std::vector<RenderedSpan> spans;
    int target_line = -1;  // TOC entries: the heading's line (clicking jumps there)
};

// A heading's title with its markup stripped ("Heading *one*" -> "Heading one").
std::string InlinePlainText(const std::vector<Inline> &ins);

// One bibliography entry in the export's wording, in three parts so a
// renderer can set the title in italics: "Knuth (1984). " + "Literate
// Programming" + ", The Computer Journal."
struct BibEntryParts {
    std::string lead, title, rest;
};
BibEntryParts BibEntry(const Citation &c);

// "Contents" and one line per heading of this document (not imported
// ones), indented by depth; lines are cut to `width` columns.
std::vector<RenderedLine> RenderToc(const Document &doc, int width);
// "References" and "[n] entry" for every cited key, in first-cited order,
// wrapped to `width` columns with a hanging indent.
std::vector<RenderedLine> RenderBibliography(const Document &doc, int width);

// Rendered citation labels (natbib-ish): "Author (1999)" / "(Author, 1999)".
std::string CiteLabel(const Document &doc, const std::string &key, bool parenthetical);

// Resolve a colour name or #rgb/#rrggbb to 0xRRGGBB; false if unknown.
bool ParseColor(const std::string &name, std::uint32_t *rgb);
// The colour names ParseColor knows, in its own order.
const std::vector<std::string> &ColorNames();
// A heading's anchor as the HTML export writes it (`id="..."`) and a
// `[text|#anchor]` link names it: lowercase, runs of anything but letters
// and digits folded to one '-'.
std::string HeadingSlug(const std::string &title);

// ---------------------------------------------------------------------------
// Export.

struct HtmlOptions {
    bool standalone = true;  // wrap in <html> with styles and MathJax
    std::string base_dir;    // image paths are made relative to this
};
std::string ToHtml(const Document &doc, const HtmlOptions &opts = HtmlOptions());

// ---------------------------------------------------------------------------
// What the exports show of a code block, as org-babel's :exports says it:
// `code`, `results`, `both` (the default) or `none`. The document's header
// sets it for every block (`//? Exports: results`, or an `exports` option);
// a block's own `exports=` wins, and so does knitr's `echo=false` (results
// only). The editor always shows everything.
void CodeExports(const Document &doc, const Block &b, bool *code, bool *results);
// Parallel to doc.blocks: true for a block the exports leave out -- one
// between the markers of `results=markdown` results that are not exported.
std::vector<bool> ExportHidden(const Document &doc);

// ---------------------------------------------------------------------------
// Tables: a table line's cells, as [begin, end) byte ranges of `line`,
// split on the pipes that are neither escaped nor inside `verbatim`. Outer
// pipes are optional, as in GitHub-flavoured Markdown: `| a | b |` and
// `a | b` are both two cells. Empty when the line has no such pipe.
std::vector<std::pair<int, int>> TableCells(const std::string &line);

// ---------------------------------------------------------------------------
// Code-block results: the lines that should replace a code block's results
// region (or be inserted right after its closing fence when it has none).
// `format` is the results' kind ("" for text, "html", "markdown"), written
// on the opening marker (`// result_begin: html`) so the results keep it.
// Markdown lines are written without the `// ` prefix, to be parsed as the
// document's own.
std::vector<std::string> FormatResults(const std::string &output, const std::string &format = "");
// The results kind a code block's options ask for: "html" for results=html
// (or output=html); "markdown" for results=markdown, md, asis (knitr's
// name) or raw (org's); "" otherwise.
std::string ResultFormatFor(const Block &b);
// An html result as something that can sit inside a page: a whole
// document's <body> content, preceded by its <head>'s <style> elements; a
// fragment unchanged.
std::string HtmlResultFragment(const std::string &html);
// For block b: the half-open line range [first, last) of `lines` that
// FormatResults' output replaces. first == last means "insert at first".
void ResultsReplaceRange(const Block &b, int *first, int *last);

}  // namespace mepml

#endif  // MEP_MEPML_DOC_H
