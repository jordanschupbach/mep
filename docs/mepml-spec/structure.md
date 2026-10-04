# mepml structure: the element tree

Parsing a mepml document yields a tree of **elements**. An element has

- a **name** (`heading`, `box`, `bold` ...),
- **attributes**, name/value strings (`level="2"`, `kind="definition"`),
- **children**, and source positions (0-based line, byte column).

Style sheets select elements by name, attributes and ancestry
([style.md](style.md)); renderers draw them ([rendering.md](rendering.md)).
Nothing in this tree says how anything looks.

`mep-mepml tree FILE` prints the tree as JSON, so a tool can consume mepml's
structure without its own parser.

## 1. Block elements

The root is `document`. Containers hold other blocks; the rest hold inline
content or nothing.

| Element | Attributes | Contains | Source |
| --- | --- | --- | --- |
| `document` | `type` = `document` \| `presentation` | blocks | the file |
| `header` | | `meta` | a run of consecutive `//?` lines |
| `meta` | `key` (lowercase: `title`, `author`, `option` ...); an option also `type` = `int` \| `double` \| `string` | — | `//? Key: value` |
| `heading` | `level` 1-6 | inlines | `> Title` ... `>>>>>> Title` |
| `paragraph` | | inlines | prose |
| `comment` | | — | `// text` |
| `callout` | `kind` (lowercase keyword: `note`, `warning`, `todo` ...) | inlines | `// NOTE: text` -- a comment with a keyword: shown where the source is edited, written by no export |
| `abstract` | | inlines | `\abstract( ... )` |
| `slide` | `number`; `title` on a presentation's title page | blocks | `\slide(` ... `)` |
| `box` | `kind`: `definition`, `theorem`, `lemma`, `proposition`, `corollary`, `fact`, `example`, `remark`, `proof`, `note`, `tip`, `warning`, or a name of the document's own | blocks | `\definition(Title,` ... `)`; `\boxed(kind, Title,` ... `)` |
| `list` | | `list-item` | `- item`, `1. item` |
| `list-item` | `ordered`, `checked` = `true` \| `false` (task items only); † `number`, `indent` | inlines | |
| `table` | | `table-cell` | pipe tables |
| `table-cell` | `header`; † `align` = `left` \| `center` \| `right`, `row`, `column` | inlines | |
| `code` | `lang` | — | fenced block |
| `results` | `format` = `text` \| `html` \| `markdown` \| `terminal` \| `gui` | — (markdown: blocks) | `// result_begin:` ... `// result_end` |
| `math-block` | | — | `$$ ... $$`, `\[ ... \]` |
| `image` | | — | `\image(path)` |
| `caption` | `of` = `figure` \| `table` \| `math` \| `code` | inlines | `\caption(...)` under a block |
| `alt-text` | | — | `\alttext(...)` |
| `rule` | | — | `---` |
| `toc` | | generated | `\toc` |
| `bibliography` | | generated | `\bibliography` |
| `import` | | — | `\import(path)` |
| `citation` | | — | `\citation(...)`, BibTeX entries |
| `define` | | — | `\define(name(params), template)` |
| `raw` | `formats` | — | `\raw(formats, text)` |
| `command` | `name` | inlines | a call of a user command |

A boolean attribute (`ordered`, `header`, `missing`) is present with an
empty value when true and absent when false.

Attributes marked † are in the tree (`mep-mepml tree`) but not offered to
selectors: a sheet styles kinds of thing, not one list item or one link.

`header` is selected for its box (the card behind a run of `//?` lines);
the `meta` lines in it are selected on their own (`meta[key=title]`), not
through it.

`slide` and `box` are the only block containers a document author nests:
`document > slide > box > paragraph` is a typical path. A `caption` and an
`alt-text` are children of the block they are written under.

## 2. Inline elements

| Element | Attributes | Source |
| --- | --- | --- |
| `bold` | | `*x*` |
| `italic` | | `~x~` |
| `underline` | | `_x_` |
| `superscript` | | `^x^` |
| `subscript` | | `,,x,,` |
| `small` | | `<x>` |
| `big` | | `>x<` |
| `mono` | | `\|x\|` |
| `highlight` | | `=x=` |
| `strike` | | `-x-` |
| `insert` | | `+x+` |
| `delete` | | `!x!` |
| `verbatim` | | `` `x` `` |
| `link` | † `href` | `[text\|url]`, `[url]` |
| `footnote` | † `number` | `\fn(text)` |
| `cite` | `parenthetical`, `missing`; † `key` | `\cite(key)`, `\citep(key)` |
| `math` | `display` | `$x$`, `\(x\)` |
| `comment` | | `text // comment` |
| `raw` | `formats` | `\raw(formats, text)` |
| `command` | `name` | `\name(args)` |
| `font` | `family` | `\f(family, text)` |
| `font-size` | `pt` | `\fs(pt, text)` |
| `color` | `value` | `\color(c, text)` |
| `span` | `class` | `\class(name, text)` |

A `span` is text with a name and nothing else: `\class(term, group)` is
`span[class=term]`, which a sheet selects as `.term`. It has no look until
a sheet gives it one.

`font`, `font-size` and `color` are **presentational markup**: the author
asked for that face, size or colour in the document itself. They are
elements like any other, but their effect is not taken from a sheet (see
rendering.md §4).

### Kinds of the document's own

The nine box kinds with a command each (`\definition(` ...) are only the
ones the default sheet has a look for. `\boxed(kind, Title,` ... `)` is a
box of any kind — `kind` a letter followed by letters, digits and `-` — and
is the same element, `box[kind=...]`. Until a sheet names it, its label is
the kind with a capital and its hyphens as spaces (`key-result` → "Key
result"), and it takes the default box's colours.

## 3. Parts

Some things a renderer draws are not in the document's text: a box's
"Definition" label, a list's bullet. Others are the document's syntax
characters, shown or hidden. These are **parts** of an element, selected
with `::name`:

| Part | Of | Is |
| --- | --- | --- |
| `::markup` | any element | its syntax characters (`*`, `\color(red,`, `>>`), when a renderer shows them |
| `::label` | `box`, `abstract`, `slide`, `caption`, `callout`, `footnote`, `code`, `results` | generated text naming it ("Definition", "Figure 3", a footnote's number, a code block's language chip) |
| `::title` | `box`, `toc`, `bibliography`, `code`; `slide` *(HTML decks)* | its title text (a box's title, "Contents", a code block's caption in its title bar, a slide's first heading) |
| `::option` | `code` | the option chips in its title bar |
| `::button` | `code` | the controls in its title bar (run, stop, fold), where a renderer has them |
| `::marker` | `list-item` | the bullet, number or checkbox |
| `::end` | `box`, `slide` | what closes it (a proof's tombstone, the rule under an open slide) |
| `::key` | `meta` | the `Key` of a header line |
| `::value` | `meta` | its value (a title or subtitle is the `meta` element itself) |
| `::name` | `meta[key=option]` | the option's name, before its `=` |
| `::rule` | `table` | its grid lines |
| `::header` | `code`, `results`, `slide` | the band its title and controls sit in |

A part inherits from its element.

A block that stands for something drawn elsewhere — `\image(path)`,
`\import(path)`, `\toc`, `\bibliography` — has its whole source line as
its `::markup`. The entries of a `toc` are `heading[level]` elements inside
it, those of a `bibliography` hold `cite` and `italic`, so a sheet's rules
for those reach them (`toc heading[level=1] { ... }`).

### The title page

A presentation (`//? Type: presentation`) shown as slides starts with a
title page made from its header. It is a `slide[title]` (its `number` is
`0`) holding the header's own elements: `meta[key=title]`,
`meta[key=subtitle]` and `meta[key=author]`, with the date an `italic`
inside the author's.

## 4. Numbering

Counters are structure, not style: a renderer does not choose them.

- `slide[number]`: 1-based, document order.
- Figures (an `image`, or a `code` block that produced one) share one
  sequence; tables have their own. Blocks an export leaves out are not
  numbered.
- `footnote[number]`: 1-based, document order.

A sheet decides the words around a number (`content: "Figure %n"`), never
the number.

## 5. What is not structure

`\define`, `\when`, `\otherwise` and `\raw` are resolved as text before an
export parses the document; an editor sees the document as written. Which
`\name(` is a box, a slide or a user command is decided by the parser alone
— a style sheet cannot add syntax.

## 6. Accessibility

What a document says to a reader who cannot see it is structure too, and
every renderer and export carries it as far as its format can:

- **Language.** `//? Lang: en-GB` (a BCP 47 tag; `Language:` too) names the
  language the document is written in. With `//? Title:`, it is what a
  screen reader announces first and picks its voice by.
- **Alternative text.** An `alt-text` (`\alttext(...)`) stands for the
  block it is written under:
  - under an `image`, or a `code` block that drew a figure, it is read in
    the picture's place;
  - under a `math-block`, or right after inline `math`, it is how the
    formula is said aloud (without one a renderer says the TeX itself,
    command by command: `\hat{\theta}_1 = \frac{a}{b}` is "theta hat 1
    equals a over b");
  - under a `table`, it says what the table shows -- a description, read
    before the cells, not instead of them.
- **Decoration.** An empty `alt-text` (`\alttext()`) under an `image` says
  the picture only decorates the page: it is left out of what is read. This
  is different from no `alt-text` at all, which leaves the picture
  undescribed.
- **Header cells.** The rows of a `table` above its `|---|` line are its
  `table-cell[header]`s: each heads its column, and a body cell is read
  under it.
- **Captions** (`caption`) are read with what they caption.

A renderer must not show an `alt-text` to a reader as content of the page
(mep's editor shows it in a popup while the cursor is on the block it
describes, because the editor is where it is written); an export writes it where its format keeps such text -- an `alt`
attribute, a structure element's `/Alt`, a picture's description.

`mep-mepml a11y FILE [read|tree|check]` prints a document as a screen
reader meets it, its structure, or what such a reader is missing -- for
`.mepml` and for the exports (`.pdf`, `.html`, `.docx`, `.odt` ...), so the
two can be compared.
