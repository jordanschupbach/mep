# mepml style sheets (`.mepss`)

A style sheet is a list of rules. Each rule selects elements of the tree in
[structure.md](structure.md) and declares properties for them. The syntax
is CSS's; the selectors and properties are a small, closed set chosen so
that a text editor drawing on a character grid and an exporter writing a
page can both honour them.

```css
/* Definitions in green, with a different word. */
box[kind=definition]        { --accent: #2e8b57; }
box[kind=definition]::label { content: "Def."; }

heading[level=1] { font-size: 1.8; color: theme(Blue, #0b5cad); }
slide heading    { text-align: center; }

@media present { verbatim { color: #d95f0e; } }
```

## 1. Syntax

```
sheet       = { rule | media } ;
media       = "@media" media-list "{" { rule } "}" ;
media-list  = media-term { "," media-term } ;
media-term  = [ "not" ] name ;
rule        = selector { "," selector } "{" { declaration ";" } "}" ;
declaration = property ":" value ;
```

- Comments are `/* ... */`.
- Names are letters, digits, `-` and `_`; they are case-sensitive and all
  defined names are lowercase.
- Strings are `"..."` or `'...'`, with `\"`, `\\` and `\XXXX` (hex code
  point, ended by a space) escapes.
- The last declaration's `;` is optional.

**Errors never stop a sheet.** An unknown property or a value a property
does not accept drops that declaration; a malformed selector drops its
rule; an unknown `@` rule is skipped with its block. Each is reported with
its line and column.

## 2. Selectors

| Selector | Matches |
| --- | --- |
| `name` | elements with that name |
| `*` | any element |
| `[attr]` | elements with the attribute |
| `[attr=value]` | ... whose value is exactly `value` (quotes optional for names and numbers) |
| `.name` | elements with that class: `[class=name]` for short (`\class(name, text)`) |
| `::part` | that part of the element (structure.md §3); last in a selector |
| `:state` | elements in that state of the renderer (below) |
| `A B` | `B` with an ancestor `A` |
| `A > B` | `B` whose parent is `A` |
| `A, B` | either |

Simple selectors combine without spaces: `box[kind=proof]::end`.
There are no ids: a mepml document has none.

States are the renderer's, not the document's. One is defined:

| State | Is |
| --- | --- |
| `:active` | the block the reader is in — in an editor, the one holding the cursor |

A renderer with no such notion (an export) never matches a state.

**Specificity** is the pair (attribute and state selectors, names) counted
over the whole selector, compared left to right; `*` counts nothing and a
`::part` counts as a name.

## 3. Cascade

Sheets apply in this order, later ones overriding earlier ones:

1. the renderer's built-in default sheet;
2. the user's sheet (mep: `~/.config/mep/mepml.mepss`);
3. each sheet the document names, in order: `//? Style: file.mepss`
   (resolved against the document's directory; sheets named by a file the
   document `//? Import:`s come before the document's own);
4. each sheet written in the document, in order: the text of a
   `\raw(style, rules ...)` block (an imported file's too, where it is
   imported). `style` is a format no export has, so the block itself is in
   none of them.

For one element and one property, among the declarations whose selector
matches and whose `@media` applies, the winner is the one from the latest
sheet in that order; within a sheet, the one with the highest specificity;
then the one written last. There is no `!important`.

A property with no winning declaration takes its parent's computed value if
the property is **inherited**, else its **initial** value. `inherit` as a
value takes the parent's computed value for any property; `initial` takes
the initial value.

## 4. Media

`@media a, b { ... }` applies when the renderer's medium has tag `a` or
tag `b`; `not a` applies when it does not. The tags are the ones `\when()`
uses in a document:

| Renderer | Tags |
| --- | --- |
| an editor's document view | `editor`, `screen` |
| ... a line shown as its source (structure.md `::markup` visible) | `editor`, `screen`, `source` |
| ... of an Org or Markdown document (mep draws those from the same sheet, their constructs as mepml's elements) | `editor`, `screen`, and `org` / `md` |
| an editor's presentation view | `present`, `slides`, `screen` |
| HTML | `html`, `screen` (+ `slides` for a deck) |
| LaTeX / PDF | `pdf`, `tex`, `latex`, `print` (+ `beamer`, `slides`) |
| Word, Writer, RTF | `docx` / `odt` / `rtf`, `print` |
| PowerPoint, Impress | `pptx` / `odp`, `slides` |
| Markdown, Org, text | `md` / `org` / `txt` |

*(planned)* `dark` and `light`, where the renderer knows which it is.

## 5. Values

- **Number**: `1.3`, `0.85`.
- **String**: `"Definition"`.
- **Keyword**: as listed per property.
- **Colour**:
  - `#rgb`, `#rrggbb`, `#rrggbbaa`;
  - a mepml colour name (`red`, `teal` ... — the names `\color()` takes);
  - `theme(Group)` or `theme(Group, fallback)`: the colour the renderer's
    own colour scheme gives `Group` (mep: a highlight group such as `Blue`,
    `Comment`, `OrgHeadlineLevel1`), and `fallback` in a renderer that has
    no such scheme (every export). This is how the default sheet follows
    the editor's theme;
  - `fade(colour, alpha)`: the colour at `alpha` (0-1) opacity over what
    is behind it;
  - `none` / `transparent`.
- **`var(--name)`** / `var(--name, fallback)`: the value of a custom
  property. Custom properties (`--accent: #2c7fb8`) are inherited, hold any
  value, and are substituted before the property is parsed.

## 6. Properties

Inh. = inherited. Media: **E** editor, **P** presentation view, **X**
exports (as far as the format can; see rendering.md §5).

### Text

| Property | Values | Initial | Inh. | Media |
| --- | --- | --- | --- | --- |
| `color` | colour | renderer's text colour | yes | E P X |
| `background` | colour | `none` | no | E P X |
| `font-weight` | `normal` \| `bold` | `normal` | yes | E P X |
| `font-style` | `normal` \| `italic` | `normal` | yes | E P X |
| `text-decoration` | `none` \| `underline` \| `line-through` \| both | `none` | yes | E P X |
| `text-decoration-color` | colour | the text's colour | yes | E P X |
| `font-size` | number (× the parent's size) \| `Npt` (N/12 × body text) | `1` | yes | E P X |
| `font-family` | `serif` \| `sans` \| `mono` \| `body`, optionally preceded by family names for exports: `"Fira Sans", sans` | `body` | yes | E P X |
| `vertical-align` | `baseline` \| `super` \| `sub` | `baseline` | yes | E P X |
| `text-align` | `left` \| `center` \| `right` | `left` | yes | E P X |

`text-decoration` inherits, unlike CSS: mepml's `+inserted+` and `[links]`
are underlined through whatever they contain. Each keyword adds to what is
inherited; `no-underline` and `no-line-through` take one away and `none`
both. `vertical-align` inherits for the same reason: what a superscript
holds is raised with it.

`background` on an inline element is behind its text, through the inlines
it holds; on a block it is the paper of the block's box (below).

### Generated text

| Property | Values | Initial | Applies to |
| --- | --- | --- | --- |
| `content` | string \| `none` | `none` | parts: `::label`, `::marker`, `::end`; `rule` |

In `content`, `%n` is the element's number (structure.md §4: a slide's,
a figure's, a table's), `%k` its kind as the document writes it (a
callout's `NOTE`, a box's `definition`), `%t` its title where it has one
(a slide's), and `%%` a percent sign. `content` replaces what the
renderer would draw for that part; `none` on a part keeps the renderer's
own text (a numbered item's number).

| Part | Default `content` |
| --- | --- |
| `list-item::marker` | `"•"`; `"□"` / `"✓"` for a task; none for a numbered item |
| `box[kind=...]::label` | the kind's name: `"Definition"`, `"Theorem"` ... |
| `box[kind=proof]::end` | `"∎"` |
| `callout::label` | `" %k "` |
| `abstract::label` | `"Abstract"` |
| `slide::label` | `"Slide %n"` |
| `caption[of=figure]::label`, `caption[of=table]::label` | `"Figure %n"`, `"Table %n"` |
| `rule` | `"─"`, repeated across the line |

### Boxes

For block elements a renderer draws a box around (`box`, `slide`, `code`,
`results`, `abstract`, `header`, `table`, `toc`, `bibliography`).

| Property | Values | Initial | Inh. |
| --- | --- | --- | --- |
| `background` | colour | `none` | no |
| `border-color` | colour | `none` | no |
| `border-left-color` | colour — a rule down the box's left edge | `none` | no |

Two parts of such a block take a `background` of their own: `::header`,
the band its title sits in (`code`, `results`, `slide`), and `::label`, the
chip naming its kind (a code block's language, "Slide 3"). A `::label`
with no `background` is drawn in its `color`; with one, its `color` is the
colour of the words on it. A code block's `::title` and `::option` take a
`color`: its title, and the outline and names of its option chips. The
chips' values are drawn in `::header`'s `color`, and the title bar's
controls in `::button`'s.

A `table`'s `background` is the colour its header and stripes are shades
of (an editor) or the paper behind it (a page); its grid lines are
`table::rule`'s `color`.

*(planned)* `margin-top`, `margin-bottom`, `padding`, `border-radius`,
`width`, `display: none`.

## 7. The default sheet

`assets/mepml/default.mepss` is normative for mep: it *is* mep's default
look. Another renderer's default sheet may differ, but it should give
every element in structure.md a look that tells it from the others.
