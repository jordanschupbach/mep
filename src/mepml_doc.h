#ifndef MEP_MEPML_DOC_H
#define MEP_MEPML_DOC_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mepml_element.h"
#include "mepml_style.h"

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
    Class,        // \class(name, x): `arg` is the name a style sheet selects
                  // it by (`.name`); no look of its own
    Footnote,     // \fn{x}; `number` is its 1-based document order
    Cite,         // \cite{key}   textual: Author (Year)
    CiteP,        // \citep{key}  parenthetical: (Author, Year)
    Math,         // $x$ or \(x\); `text` is the TeX, `alt` any \alttext();
                  // `arg` is "display" for an inline $$x$$
    Comment,      // `text // comment` to the end of the line (not rendered)
    Raw,          // \raw(formats, text): `arg` the formats it is for, `text`
                  // written into those exports verbatim (see ExpandCommands)
    Command,      // \name(args) calling a user command (\define): `arg` the
                  // name, `text` the arguments as written, `children` them
                  // parsed as prose -- how the editor shows the call
};

struct Inline {
    InlineKind kind = InlineKind::Text;
    std::string text;  // Text/Verbatim/Math contents, Cite key
    std::string arg;   // Link url, Font family, FontSize size, Color colour
    std::string alt;   // Math \alttext()
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
    Import,        // \import(path) (or @import{path})
    Citation,      // \citation(key, field = value, ...), @citation{key}{...} or BibTeX
    MathBlock,     // $$ ... $$  or  \[ ... \]
    Code,          // ```{lang, opts} ... ```
    Image,         // \image(path) (or @image{path})
    Table,         // | a | b |
    List,          // - item / 1. item / - [ ] task
    Rule,          // --- (3+ of - = _ *)
    Bibliography,  // \bibliography (@bibliography and @printbibliography still parse)
    TableOfContents,  // \toc (or @toc)
    Abstract,      // \abstract( prose, blank lines between paragraphs )
    // A slide: `\slide(` on a line of its own opens it (`\slide{` and
    // `@slide{` too), a line holding just its closing `)` (`}`) ends it.
    // What lies between is the slide's content, ordinary blocks of the
    // document, so the two markers are blocks of their own. `level` is the
    // slide's 1-based number on both. A slide left open ends where the
    // next one starts, or at the end of the document.
    SlideBegin,
    SlideEnd,
    // \define(name(params), template): a user command. `keyword` is its
    // name, `field_order` its parameters, `code` the template and
    // code_line_start..code_line_end the lines it spans. Exports expand
    // the calls and drop the definition (ExpandCommands).
    Define,
    // \raw(formats, text) on lines of its own: `lang` the formats, `code`
    // the text, written verbatim by those exports and left out of others.
    Raw,
    // \name(args) on lines of its own, calling a user command (a paragraph
    // may hold calls too, as InlineKind::Command): `keyword` the name,
    // `value` the arguments as written, `inlines` them parsed as prose.
    Command,
    // A titled box -- `\definition(Title,` on a line of its own opens one
    // (or \theorem, \lemma ...: see BoxKinds), a line holding just `)`
    // closes it, and what lies between is its content, ordinary blocks of
    // the document, as on a slide. Boxes nest, and close before the slide
    // they are on. BoxBegin: `keyword` the kind, `caption` /
    // `caption_inlines` the title (before the first top-level comma; empty
    // for `\proof(`), `inlines` any text after the comma -- the box's
    // first paragraph, over as many lines as it runs -- and `box_closed`
    // when its `)` ends that text (`\remark(Title, text)` on one line):
    // then no BoxEnd follows. Without a comma, a box closed on its opening
    // line is all text (`\remark(text)`) and any other's opening line is
    // its title. BoxEnd: `keyword` the kind of the box it closes. `level`
    // is the box's depth on both, 1 for one not inside another.
    BoxBegin,
    BoxEnd,
    // Columns: `\columns(` on a line of its own opens a row of columns and
    // each `\column(` inside it one column -- both closed by a line holding
    // just `)`, their content ordinary blocks, as in a box. The columns
    // share the width equally unless one names its own (`\column(40%,`).
    // LayoutBegin / LayoutEnd: `keyword` is "columns" or "column", `value`
    // a column's width as written ("40%", "" for an equal share) and
    // `level` the depth among the open boxes and columns.
    LayoutBegin,
    LayoutEnd,
    // Markup written into the document as it is: a line that starts with
    // `<svg` opens an Svg block, one that starts with an HTML block tag
    // (`<div`, `<table`, `<details`, `<figure`, a doctype ...) an Html
    // block, and either runs to the line on which that element closes --
    // its `</tag>` balancing the `<tag`s inside it, blank lines and all.
    // One never closed ends before the first blank line after it. `keyword`
    // is the opening tag ("svg", "div"), `code` the markup (its lines,
    // code_line_start..code_line_end), and a \caption / \alttext under it
    // attaches as to an image: an Svg is a figure, an Html one when it has
    // a caption. The editor draws the markup in place; HTML exports write
    // it as it is.
    Svg,
    Html,
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
    // A cell holding nothing but `\image(path)` is a picture: the path,
    // "" for an ordinary cell. `content` still has the raw text.
    std::string image;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int line_start = -1, line_end = -1;  // inclusive
    // Non-empty for a block that came from an \import: the imported file's
    // path. Line numbers are then that file's, not the buffer's.
    std::string origin;

    // The block's source text, lines joined with '\n' (exactly the buffer
    // bytes from line_start's column 0 to line_end's end). Inline offsets
    // index this; OffsetToPos maps them back to (line, col).
    std::string text;
    std::vector<Inline> inlines;  // Paragraph, Heading title, Callout body, captions

    int level = 0;               // Heading; SlideBegin/SlideEnd: the slide's number
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
    // Result lines that are a figure the block produced (`// \image(path)`,
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

    // Caption / alt text attached by following \caption() / \alttext()
    // lines (Image, Table, MathBlock, Code). Either may run over several
    // lines (`\caption(` ... `)`): *_line is where it opens, *_line_end
    // where its closing brace is, at column *_close_col. Line breaks in
    // `caption`/`alt` and in caption_inlines' text are single spaces.
    std::string caption, alt;
    std::vector<Inline> caption_inlines;
    int caption_line = -1, alt_line = -1;
    int caption_line_end = -1, alt_line_end = -1;
    int caption_close_col = -1, alt_close_col = -1;

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

    bool box_closed = false;  // BoxBegin: closed on its own lines (see BlockKind::BoxBegin)

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

// A user command, from `\define(name(p1, p2), template)`. A call
// `\name(a1, a2)` becomes the template with `#p1`/`#1` replaced by the
// arguments; the last parameter takes the rest of the call, commas and
// all, as the built-in commands' text does.
struct UserCommand {
    std::string name;
    std::vector<std::string> params;
    std::string body;
    int line = -1;       // the \define's line
    std::string origin;  // "" for this document, else the imported file
};

// A style sheet the document names: `//? Style: file.mepss`
// (docs/mepml-spec/style.md §3).
struct StyleRef {
    std::string path;  // as written
    std::string base;  // the imported file that named it; "" for the document itself
    int line = -1;     // the header line (the import's line, for an imported file's)
};

struct Document {
    std::string title;
    std::vector<std::pair<std::string, std::string>> meta;  // every //? Key: value, in order
    std::vector<Option> options;                             // document-level //? Option:
    std::vector<Block> blocks;
    std::map<std::string, Citation> citations;
    std::vector<std::string> cite_order;  // keys in first-cited order
    std::map<std::string, UserCommand> commands;  // every \define, imports' too
    // The sheets that style it, in cascade order: those its imports name
    // first, then its own.
    std::vector<StyleRef> styles;
    // Those sheets read and parsed, after the user's own (LoadStyleSheets;
    // ParseForExport fills it): what an export styles the document with,
    // over mep's built-in look. Empty for a document parsed any other way.
    std::vector<std::shared_ptr<const style::Sheet>> sheets;
    // What ParseForExport expanded the document for (empty: as written).
    // The HTML export writes a \raw for HTML as it is and wraps any other
    // (LaTeX, on its way through HTML to a .tex) for the next step.
    std::vector<std::string> export_tags;
    int footnote_count = 0;
    std::vector<Diagnostic> diagnostics;

    const Option *FindOption(const std::string &name) const;
    // Index into blocks of the block covering `line`, or -1.
    int BlockAtLine(int line) const;
};

// A \abstract block's paragraphs, each its own run of inlines.
std::vector<std::vector<Inline>> AbstractParagraphs(const Block &b);

// One slide of the document (not of an \import): its number, the lines
// from its opener to its last line -- the closing bracket's, or for a
// slide left open the line before the next opener (or the document's
// last line) -- its blocks as [first, last) indices into doc.blocks,
// markers included, and its title: the text of its first heading, "" if
// it has none.
struct Slide {
    int number = 0;
    int line_start = -1, line_end = -1;
    bool closed = false;
    size_t first_block = 0, last_block = 0;
    std::string title;
};
std::vector<Slide> Slides(const Document &doc, int line_count);

// A header value by key, compared without case (`//? author:` is
// `Author`); the last one wins, "" when there is none.
std::string MetaValue(const Document &doc, const std::string &key);
// The language the document is written in, a BCP 47 tag ("en", "en-GB",
// "fr"): its `//? Lang:` header (`Language:` too), "" when it names none.
// Every export that has a place for it says so -- a screen reader picks its
// voice by it.
std::string DocumentLanguage(const Document &doc);
// `//? Type: presentation` (or `slides`): the document is a slide deck.
// Its HTML, LaTeX and PDF exports are then a slideshow and a Beamer deck,
// holding only what is on its slides, after a title slide made from the
// header (Title, Subtitle, Author, Date). `//? Type: document` is the
// default.
bool IsPresentation(const Document &doc);

// What `//? Export:` may name -- the format the Run button exports to and
// opens -- each with a line describing it, canonical names first; the
// aliases (markdown, latex, text, powerpoint, impress) follow. `beamer`
// is the Beamer deck's PDF whatever the document's Type.
struct ExportFormat {
    const char *name;
    const char *doc;
};
const std::vector<ExportFormat> &ExportFormats();

// The callout keywords a `// KEYWORD:` comment recognises.
const std::vector<std::string> &CalloutKeywords();

// The kinds of titled box (BlockKind::BoxBegin): the command's name, the
// label every rendering puts before the title ("Definition"), and its
// colours in an export with no style sheet -- `color` the accent
// (#rrggbb), `tint` the paper behind the content. (The editor's look is
// the default sheet's: assets/mepml/default.mepss.) A proof is quieter
// than the rest and ends with a tombstone (∎).
struct BoxKind {
    const char *name;
    const char *label;
    const char *color;
    const char *tint;
};
const std::vector<BoxKind> &BoxKinds();
// The kind named `name` ("definition"), nullptr for anything else.
const BoxKind *FindBoxKind(const std::string &name);
// A column's width (`\column(40%,`) as a percentage of its row, 0 for a
// column that takes an equal share of what the others leave.
int ColumnPercent(const Block &b);
// How wide each column of a row is where text is set in a grid of
// `total_cols` columns (the editor, its presentation view), from their
// ColumnPercent()s: `gap` columns between neighbours, a named width its
// share of the rest, the others what is left in equal parts.
std::vector<int> ColumnCols(const std::vector<int> &percents, int total_cols, int gap = 2);
// A box of any kind is written `\boxed(kind, Title,` ... `)`: the kind is a
// name of the document's own (`axiom`, `key-result`), and how it looks --
// its label, its colours -- is the style sheets' (`box[kind=axiom]`). The
// kinds above are the ones with a command of their own (`\definition(`)
// and a look in the default sheet.
// Whether `name` can be a box's kind: a letter, then letters, digits, `-`.
bool IsBoxKindName(const std::string &name);
// The label a kind has before any sheet names it: a built-in kind's own
// ("Definition"), any other's name with a capital ("Axiom").
std::string BoxLabel(const std::string &kind);
// The text that opens a box of `kind`, up to where its title starts:
// `\definition(` or `\boxed(axiom, `.
std::string BoxOpenerText(const std::string &kind);
// "Definition: Title", "Definition" without one -- a box's heading as the
// exports without a box of their own (Markdown, plain text ...) write it.
std::string BoxHeading(const Block &b);
// The same, its label the one `doc`'s style sheets give the kind.
std::string BoxHeading(const Document &doc, const Block &b);

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
// \import resolution. `read` returns false when the file cannot be read.
// Imports are expanded in place (their blocks' line numbers stay those of
// the imported file; `origin` in the result names which file). Cycles and
// missing files become diagnostics on the \import line.
using ReadFileFn = std::function<bool(const std::string &path, std::vector<std::string> *lines)>;
std::string ResolvePath(const std::string &base_file, const std::string &path);
Document ParseWithImports(const std::string &file, const std::vector<std::string> &lines, const ReadFileFn &read);

// ---------------------------------------------------------------------------
// User commands and export conditions.
//
// `\define(name(params), template)` makes a command; `\when(formats,
// text)` keeps its text only in the exports it names, and an
// `\otherwise(text)` right after one or more \when()s keeps its text when
// none of them matched; `\raw(formats, text)` writes text verbatim into
// the exports it names (HTML, LaTeX, Markdown ...). These are expanded as
// text, before the export parses the document -- the editor and the
// language server see the document as written.
//
// An export is a list of tags, most specific first: a PDF of an article is
// {"pdf", "tex", "latex"}, a presentation's HTML {"html", "slides"} (see
// ExportTags in mepml_convert.h). `formats` is a list of names separated by
// spaces or `|`; it matches when any name is one of the tags (`*` matches
// every export), and a `!name` excludes an export (`!html`).
bool FormatsMatch(const std::string &formats, const std::vector<std::string> &tags);
// The names \when, \otherwise and \raw understand: every tag some export
// has. Others still work -- they just never match -- so a document can
// name formats a later mep adds.
const std::vector<std::string> &KnownFormatTags();
// `lines` with the user commands called in them expanded for an export
// tagged `tags`, \when/\otherwise resolved, \raw kept only for a matching
// export and \define blocks removed. Code, maths, comments and header lines
// are left alone. Problems (a missing argument, runaway recursion) are
// appended to *errors as "line N: message".
std::vector<std::string> ExpandCommands(const std::vector<std::string> &lines,
                                        const std::map<std::string, UserCommand> &commands,
                                        const std::vector<std::string> &tags,
                                        std::vector<std::string> *errors = nullptr);
// The document as an export tagged `tags` sees it: its commands (and its
// imports' commands) expanded in it and in every file it imports.
Document ParseForExport(const std::string &file, const std::vector<std::string> &lines, const ReadFileFn &read,
                        const std::vector<std::string> &tags);

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
    kDirective = 1u << 17,  // \image(...) etc.
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
    kAbstract = 1u << 29,   // a \abstract's `\abstract(` / `)` (replace = its "Abstract" label)
    kSlide = 1u << 30,      // a \slide's `\slide(` / `)` lines (replace = "Slide N" on the opener)
    // A box's own markup (`\definition(`, the comma after its title, its
    // `)`; replace = its label on the opener) and its title; `target` is
    // the box's kind.
    kBox = 1u << 31,
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
    // What the span is, as a node of Highlight's ElementPaths (see
    // docs/mepml-spec/structure.md): `path` as it is rendered -- a box's
    // `\definition(` is the box's `::label` -- and `source_path` where a
    // renderer shows the line's source instead (there it is `::markup`).
    // How a span looks is the style computed for its node, not `style`:
    // the flags say what the span is for layout and behaviour.
    int path = -1, source_path = -1;
    int number = 0;            // kSlide: the slide's number (a `%n` in its label)
    bool block_end = false;    // kBox / kSlide: the `)` closing it, on a line of its own
};

// The spans of every block of this document (not of its imports). With
// `paths`, the element tree the spans' `path`s index is built into it;
// with `block_nodes`, each block's own node in it (parallel to doc.blocks,
// -1 for an imported block; a slide's and a box's two markers share one).
// `context` is for text that is a piece of a document rather than a whole
// one -- a page of the presentation view.
struct HighlightContext {
    // The element the text is inside ("" for none): a page is its
    // `slide[number=N]`, so `slide heading` selects on it as in the
    // document.
    Element container;
    // The text is a presentation's title page (PresentationPages writes
    // one from the header): its heading is the document's
    // `meta[key=title]`, a paragraph that is one >big< run its
    // `meta[key=subtitle]`, any other paragraph `meta[key=author]`.
    bool title_page = false;
};
std::vector<Span> Highlight(const Document &doc, ElementPaths *paths = nullptr, std::vector<int> *block_nodes = nullptr,
                            const HighlightContext *context = nullptr);
// The document as its element tree (docs/mepml-spec/structure.md), in
// JSON: every element `{"name", "attrs", "children"}` with its source
// `lines`, text as `{"text"}`. What `mep-mepml tree FILE` prints, so a tool
// can read mepml's structure without a parser of its own.
std::string ElementTreeJson(const Document &doc);
// Every element name a document's tree can hold (structure.md §1 and §2),
// blocks first.
const std::vector<std::string> &ElementNames();
// Whether an element is a block (structure.md §1) rather than inline: an
// inline's `background` is behind its text, through the inlines it holds.
bool IsBlockElement(const std::string &name);

// A results line (its `// ` prefix already stripped) that names a figure:
// `\image(path)` (or `@image{path}`). Sets *path.
bool ResultImagePath(const std::string &text, std::string *path);

// SVG and HTML written into a document (BlockKind::Svg / Html): the tag
// `line` opens a markup block with ("svg", "div", "table" ...), "" for a
// line that opens none -- an importer escapes such a line of prose.
std::string MarkupBlockTag(const std::string &line);
// The line on which the markup block opened on lines[first] closes, -1
// when it never does (or lines[first] opens none).
int MarkupBlockClose(const std::vector<std::string> &lines, int first);

// A block that is a picture of its own: an \image, an <svg>, or HTML
// with a \caption. (A code block that drew a figure is one too, where
// its results are shown: see BlockLabels.)
bool IsFigure(const Block &b);

// "Figure N" / "Table N" for every numbered block, "" for the rest,
// parallel to doc.blocks. Images and code blocks that produced a figure
// share one sequence; tables have their own. The editor's captions and
// the HTML export both number from this, so they always agree. What the
// exports leave out (see CodeExports) is not numbered.
std::vector<std::string> BlockLabels(const Document &doc);

// --- Generated content: the table of contents and the bibliography, as
// lines of styled text (the editor draws these in place of the \toc /
// \bibliography row; the HTML export builds the same content as markup).

struct RenderedSpan {
    int col_start = 0, col_end = 0;  // byte offsets into RenderedLine::text
    std::uint32_t style = 0;         // kBold / kItalic / kHeading / kComment / kCite / kLink
    int heading_level = 0;           // with kHeading
    // The span's colour once a renderer has styled it (the editor: its
    // element's computed style, as a highlight group or "#rrggbb"), with
    // kBold / kItalic in `style` set to match. Empty: not styled yet, and
    // `style` alone says what the span is.
    std::string hl;
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
// A block's caption -- `label` ("Figure 1", "" for none) in bold, then
// its text in italics with its inline styling -- and alt text (muted
// italics), word-wrapped to `width` columns, each line centred when
// `center`. The editor draws these in place of the \caption/\alttext rows.
std::vector<RenderedLine> RenderCaption(const Document &doc, const Block &b, const std::string &label, int width,
                                        bool center);
std::vector<RenderedLine> RenderAltText(const std::string &alt, int width, bool center);

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

// A link's target as an export writes it. mepml follows Org in letting a
// link name another file as `file:docs/web.org`; exports whose readers do
// not know that scheme (HTML, Markdown, DOCX, ...) get the plain relative
// path, `docs/web.org`, which resolves beside the exported file. Any
// `#anchor` is kept. Other targets are returned as written.
std::string LinkTarget(const std::string &arg);

// ---------------------------------------------------------------------------
// Export.

struct HtmlOptions {
    bool standalone = true;  // wrap in <html> with styles and MathJax
    // Where the document's relative paths start from -- its own directory
    // -- and where the HTML is written. With both set, a `file:` link's
    // path is rewritten to resolve from out_dir (build/README.html links
    // ../help/mepml.org), and so is a picture's that leaves the document's
    // directory (../x.png) or is absolute; a picture inside it keeps its
    // path (assets/logo.png), since the export copies it into out_dir at
    // that path (mepml_convert.h, CopyPictures). With either empty every
    // path is kept as the document writes it (the editor's own views).
    std::string base_dir;
    std::string out_dir;
};
std::string ToHtml(const Document &doc, const HtmlOptions &opts = HtmlOptions());
// A presentation as a self-contained HTML slideshow: a title slide and one
// 16:9 slide per \slide, scaled to the window, stepped through with the
// arrow keys (or space, a click, a swipe); `f` goes full screen, and
// printing gives one slide per page. Only the slides' content is shown.
// Not standalone: just the <section class="slide"> elements.
std::string ToSlidesHtml(const Document &doc, const HtmlOptions &opts = HtmlOptions());
// Each slide's content as HTML: `title` is its first heading's inline
// markup ("" for an untitled slide), `body` the rest of its blocks (and
// its footnotes). The slideshow and the Beamer export both build on these.
struct SlideHtml {
    int number = 0;
    std::string title, body;
};
std::vector<SlideHtml> SlideFragments(const Document &doc, const HtmlOptions &opts = HtmlOptions());

// ---------------------------------------------------------------------------
// Style sheets in exports (docs/mepml-spec/rendering.md §5). An export's
// own look is built in; the sheets a document is exported with change it
// where they say something the default sheet does not, as far as the
// format can show it -- so a document with no sheet exports exactly as it
// always has.

// The user's sheet for every document: $XDG_CONFIG_HOME/mep/mepml.mepss
// (~/.config/...), "" when neither variable is set.
std::string UserStyleSheetPath();
// Reads the user's sheet and the ones the document names (doc->styles,
// resolved against `file`) into doc->sheets; a sheet that cannot be read
// is skipped.
void LoadStyleSheets(Document *doc, const std::string &file, const ReadFileFn &read);
// The style sheets written in the document itself, in order (its imports'
// too): each `\raw(style, rules ...)` block's text. They apply after the
// sheets the header names. (`style` is a format no export has, so the
// blocks themselves are in no export.)
std::vector<std::string> InlineStyleSheets(const Document &doc);
// The names `\class(name, ...)` gives text in the document, in the order
// they first appear, each once.
std::vector<std::string> ClassNames(const Document &doc);
// A class as an export without CSS draws it: what the document's sheets
// give `.name` -- a colour ("#rrggbb", "" for none), bold, italic.
struct ClassLook {
    std::string color;
    bool bold = false, italic = false;
};
ClassLook ExportClassLook(const Document &doc, const std::string &name);
// The media tags an export of `doc` has (style.md §4): its export tags,
// with `screen` or `print` as the format is one or the other.
std::vector<std::string> ExportMedia(const Document &doc);
// A box kind as an export draws it: the label before its title, its accent
// and the paper behind it (both "#rrggbb"), and what closes it ("" for
// nothing). BoxKinds' own unless the document's sheets say otherwise.
struct BoxLook {
    std::string label, color, tint, end;
};
BoxLook ExportBoxLook(const Document &doc, const std::string &kind);
// What the document's sheets change for runs of text, for an export that
// draws from constants of its own (LaTeX through its HTML, Word, Writer,
// RTF, the slide formats): the colour, weight and slant an element gets
// from the sheets where they differ from mep's built-in look -- and only
// what the element itself brings, not what it inherits from one already
// changed. An exporter tells it where it is as it walks: ForBlock at each
// block, Enter / Leave round each inline.
class ExportTextStyler {
  public:
    struct Change {
        std::string color;  // "RRGGBB", "" for no change
        int bold = -1, italic = -1;  // 1 / 0 to set, -1 for no change
        bool any() const { return !color.empty() || bold >= 0 || italic >= 0; }
    };
    explicit ExportTextStyler(const Document &doc);
    ~ExportTextStyler();
    ExportTextStyler(const ExportTextStyler &) = delete;
    ExportTextStyler &operator=(const ExportTextStyler &) = delete;
    // False without sheets: every call then answers "no change".
    bool active() const;
    Change ForBlock(const Block &b);
    Change Enter(const Inline &x);
    void Leave();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The document's sheets as CSS for the HTML ToHtml writes: each rule's
// selector as the markup it selects there (`box[kind=proof]::label` is
// `.mbox-proof .mbox-label`), its values as CSS's. Rules the page has
// nothing for are left out. "" without sheets.
std::string ExportSheetCss(const Document &doc);

// The look the sheets -- mep's default, then the document's -- give an
// element, for an export whose format has a style table of its own (Word,
// Writer): a heading's or the title's size, weight and colour, a link's
// colour, so those tables say what the editor draws.
struct ExportTextLook {
    std::string color;  // "#rrggbb", "" for none
    bool bold = false, italic = false, underline = false;
    float font_size = 1.0f;  // × body text
};
ExportTextLook ExportLookFor(const Document &doc, const std::vector<Element> &chain);

// ---------------------------------------------------------------------------
// What the exports show of a code block, as org-babel's :exports says it:
// `code`, `results`, `both` (the default) or `none`. The document's header
// sets it for every block (`//? Exports: results`, or an `exports` option);
// a block's own `exports=` wins, and so does knitr's `echo=false` (results
// only). The editor always shows everything. `fallback` is the mode when
// neither the header nor the block names one (the presentation view uses
// "results").
void CodeExports(const Document &doc, const Block &b, bool *code, bool *results, const std::string &fallback = "both");
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

// ---------------------------------------------------------------------------
// The editor's presentation view (Editor::MepmlPresentStart): each page of
// the deck as mepml text of its own, to be drawn by the editor's ordinary
// renderer but reading like an export rather than like the source. A title
// page from the header comes first for a presentation (`//? Type:`) with a
// Title, Subtitle or Author, then one page per \slide, holding only what an
// export shows of it:
//   - user commands expanded for the tags {"present", "slides"} (so
//     `\when(present, ...)` picks out the view, and `\otherwise` applies
//     where nothing else does), \define and \raw blocks dropped;
//   - code blocks as CodeExports says, with "results" the default when
//     neither the header nor the block names one (a block with no results
//     yet then shows its code, rather than nothing): the code as a bare
//     ```lang fence without its options, text output as a plain fence
//     (PresentationPage::outputs),
//     figures as \image() lines, Markdown output as the document's own
//     text; option (`//?`) lines, comments and result markers dropped;
//   - citations as their rendered labels, \bibliography as the list of
//     references and \toc as the list of slide titles, so a page needs no
//     \import to read right.
// With `wrap_cols` > 0, paragraphs and list items -- whose line breaks read
// as spaces -- are refilled to that many columns as the editor draws them
// (markup concealed, maths about as wide as it typesets), breaking only
// between words and never inside maths, code, a link or a citation; so a
// slide reads as prose at any size rather than as the source's lines.
// A row of columns keeps its markers (`\columns(`, `\column(` and their
// `)`), which the editor sets side by side; what is in a column is filled
// to that column's width (ColumnCols of `wrap_cols`).
// A page's lines never start or end with a blank line nor hold two in a row.
struct PresentationPage {
    int number = 0;         // the \slide's number; 0 for the title page
    int source_line = -1;   // the \slide( line in `lines`, -1 for the title page
    int source_end = -1;    // the slide's last line (its closer, if any)
    std::string title;      // the slide's first heading, or the document's title
    std::vector<std::string> lines;
    // Each code block whose code the page shows: (the line of `lines` its
    // opening fence is on, the line in the document as written of the
    // block's own fence) -- what running it from the page runs.
    std::vector<std::pair<int, int>> code_blocks;
    // Every code block the page shows anything of, in order: the lines of
    // `lines` it takes (code, output, figures and caption), its fence in
    // the document as written, whether its code shows, whether it is a
    // live one -- its output a program that runs on (results=web, an
    // exec-gui window, an exec block's terminal) -- and, while that program
    // runs with a window of its own, the line of the output fence its
    // window is drawn under (-1 otherwise).
    struct Shown {
        int first = -1, last = -1;
        int source_fence = -1;
        bool code = false;
        bool live = false;
        int live_fence = -1;
    };
    std::vector<Shown> blocks;
    // Each fence of `lines` that opens a block's text output (the line it
    // is on), or is the bare fence an html result hangs under when its
    // code is hidden. The view draws what is in it as it is -- no title
    // bar, no card -- and its two fence rows empty, so no blank line is
    // beside them. (Nor has an html result a card: its markers are empty
    // rows too.)
    std::vector<int> outputs;
    // Each alt text of the slide: (the line of `lines` that ends what it
    // describes, its text -- empty for a decoration's `\alttext()`). The
    // \alttext() lines themselves are not on the page.
    std::vector<std::pair<int, std::string>> alts;
};
std::vector<PresentationPage> PresentationPages(const std::string &file, const std::vector<std::string> &lines,
                                                const ReadFileFn &read, int wrap_cols = 0);
// One line of prose (no line breaks) filled to `cols` columns as above:
// the first line after `first`, the rest after `rest` (a list item's
// marker and the indent under it).
std::vector<std::string> FillProse(const std::string &text, int cols, const std::string &first = "",
                                   const std::string &rest = "");
// How many columns a line of prose takes as the editor draws it: markup
// concealed, maths about as wide as it typesets (FillProse's measure).
int ProseColumns(const std::string &line);

}  // namespace mepml

#endif  // MEP_MEPML_DOC_H
