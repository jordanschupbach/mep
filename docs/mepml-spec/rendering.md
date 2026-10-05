# Rendering mepml

What a renderer — an editor's document view, a presentation view, an
exporter — must do to conform.

## 1. The pipeline

1. **Parse** the document into the element tree (structure.md).
2. **Collect** the sheets that apply (style.md §3) and the renderer's media
   tags (style.md §4).
3. **Compute** a style for every element and part it will draw: the cascade
   and inheritance of style.md §3, down the element's ancestors.
4. **Draw** each element with its computed style, as far as the medium
   allows (§5).

Steps 1-3 are fully determined by this specification: two conforming
renderers given the same document, sheets and media tags compute the same
styles. `mep-mepml style FILE --media TAGS` prints them, and is the
reference.

## 2. What style controls, and what it does not

Style controls appearance: colour, weight, slant, decoration, size, face,
alignment, generated text, and the boxes behind block elements.

It does not control, and a sheet cannot change:

- **structure** — which text is which element, numbering, which blocks an
  export includes (`exports=`), `\when()` conditions;
- **behaviour** — in an editor: which line shows its source (mep: the
  cursor's), folding, running code blocks, following links, completion;
- **content fetched or computed** — a formula's typesetting, an image's
  pixels, a code block's results.

## 3. Showing source

An editor may draw an element as its rendering or as its source text (mep
shows the source of the line the cursor is on, and of everything when
concealment is off). A line shown as source has the extra media tag
`source`, and its syntax characters are the `::markup` parts of its
elements. A renderer must not hide a `::markup` part on a `source` line.

### Presenting

A renderer showing one slide at a time (mep's presentation view, media
`present`) draws each page as its `slide` element: selectors with a
`slide` ancestor apply on the page as they do in the document, and the
slide's `background` is the page's paper, edge to edge, rather than a box
round its content. A page's text takes the slide's `color` by inheritance.

A code block's results are on the page as they are: text output in the
code face, an HTML result laid out, a figure as its picture -- none with
a title bar or a box of its own. (The code, where the slide shows it,
keeps its card.)

## 4. Presentational markup

`\color(c, x)`, `\f(family, x)` and `\fs(pt, x)` are the author's own
styling. Their elements' computed styles are fixed by the document and
override any sheet:

- `color[value]`: `color` is `value`;
- `font[family]`: `font-family` is `family`, mapped to the nearest of
  `serif` / `sans` / `mono` where the renderer has only those;
- `font-size[pt]`: `font-size` is `pt`/12 × body text.

Their descendants inherit from them as from any element, so a sheet's rule
for a descendant still applies.

## 5. Degrading

A medium that cannot express a property does the nearest thing it can, and
never fails:

| Cannot | Does |
| --- | --- |
| scale text (a terminal grid) | draws it at body size; a sheet can give `@media source` or that renderer its own hint |
| draw `fade()` | blends against its background colour |
| draw a face | the nearest of `serif` / `sans` / `mono`; then body text |
| draw a box (plain text, Markdown) | writes the `::label` and `::title` as a heading line |
| resolve `theme(G)` | uses the fallback; with none, the initial value |

### What mep's exports honour

An export keeps its own built-in look and applies what the document's
sheets (the user's, then the `//? Style:` ones — not the default sheet)
change in it, so a document with no sheet exports as it always has.

| Export | From the sheets |
| --- | --- |
| HTML, HTML slideshow | every rule whose selector has markup on the page, as CSS appended to the page's own (`mepml::ExportSheetCss`): all text and box properties, `content` for list markers and a box's end mark; a box's `::label` text is written into the page |
| LaTeX, PDF, Beamer | `color`, `font-weight`, `font-style` of headings, paragraphs and inline elements; a box's accent, paper, label and end mark |
| DOCX, ODT | the same three properties of headings, paragraphs and inline elements, as character styles named for the change (so the text reads back as it was written); a class's look as its own character style; a box's label, accent and paper |
| PPTX, ODP | the same three properties of inline elements, on the runs; a box's label and accent |
| RTF | a box's label, accent and paper; a class's name (RTF keeps a look on the run itself, where it would read back as the author's own markup) |
| Markdown, text | a box's label |

Not selectable in an export: `::markup`, any `:state`, `header`, `comment`
and the other elements an export leaves out. Selectors on attributes the
page does not carry (`cite[parenthetical]`, `results[format=...]`) are
skipped.

A size that changes a line's height (a large heading) is the renderer's to
lay out: mep gives the line extra rows. Sizes are clamped to 0.5-3 × body
text in mep's editor so a typo cannot fill the pane.

## 6. Conformance

A renderer conforms if, for every element and part it draws, the
properties of style.md §6 marked for its medium come from the computed
style of §1 — not from constants of its own. Its built-in look is a default
sheet, replaceable rule by rule.

## 7. Where mep's editor stops short

The editor sets text on a character grid, one source line to a row, and
that bounds what it does with a sheet:

- `text-align`: a heading, an abstract's label and running text
  (`paragraph`, `list`, `callout`, `abstract`) are set centred or flush
  right line by line, within the text width and by whole cells. The line
  the caret is on is drawn where it is typed, a line too long for the pane
  (soft-wrapped) stays on the left, and `justify` is drawn as `left`.
  Tables, code and figures keep the placement the editor gives them;
- a table's stripes: `table`'s `background` is the colour they are shades
  of, at the editor's own strengths for the header and alternate rows;
- the glyphs `content` may use: any character of the editor's text face
  (JetBrains Mono) is drawn, loaded when a sheet first uses it, as are the
  dingbats and symbols of its symbol face; one that neither has shows
  as `?`.
