# mepml: separating structure from style

Goal: mepml says *what a thing is* (a heading, a definition box, a code
block's results); a **style sheet** says *how it looks*; a written
**specification** says how a renderer combines the two. mep becomes one
implementation of that specification, and its look becomes something the
user edits in a file rather than something compiled into the editor.

The HTML/CSS split is the model: mepml = HTML (structure), `.mepss` = CSS
(style), mep = a browser (one conforming renderer).

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---------------------------------------------------------------------------

## 1. Where the coupling is today

The parser (`src/mepml_doc.cpp`) is already format-neutral: it produces a
`mepml::Document` of blocks and inlines. Everything about *appearance* is
decided in C++ in five places, each with its own hardcoded table:

| Where | What it hardcodes |
| --- | --- |
| `mepml::Highlight` (mepml_doc.cpp) | generated text: "Abstract", "Slide N", "Definition:", `•`, `□`/`✓`, `∎`, "Caption: ", callout badges |
| `Editor::MepmlScan` (editor_mepml.cpp:994-1220) | a 20-line `if (style & kX) hl = "Purple"` chain; `SpanScale` (big 1.3, small 0.8, super/sub 0.7, 12pt body); `CalloutColor`, `HeadingColor`; header title scale 2.2 / subtitle 1.15; alt-text scale 0.85; rule glyph `─`; table `│`; bold/italic/underline policy per construct |
| `Editor::MepmlBuildCards` + main.cpp card painter (≈49125-49345, 52271-52712) | which constructs get a card, wash opacity (0.07/0.09), border fade, left rule on boxes, `"Cyan"`/`"Purple"` fallbacks, chips |
| `kOrgHeadingStyles` (editor.h) | heading scale + row slots per level, shared with org |
| `mepml::BoxKinds`, `kCss`, `BoxCss`, `kSlidesCss`, doc_export.cpp LaTeX colours, mepml_export.cpp DOCX/ODT styles, mepml_slides.cpp | every export's look, each from its own constants |

Nothing a user writes can change any of it.

## 2. Target architecture

```
  .mepml text ──parse──▶ Document ──▶ ELEMENT TREE ─┐
                                                     ├─cascade─▶ computed styles ─▶ renderer
  default.mepss ◀ user.mepss ◀ //? Style: doc.mepss ─┘                 (mep editor, presentation
                                                                        view, HTML, LaTeX, DOCX ...)
```

Three artefacts, each specified in `docs/mepml-spec/`:

1. **Structure** (`structure.md`) — the element vocabulary. Every construct
   is a named element with attributes, e.g. `heading[level=2]`,
   `box[kind=definition]`, `callout[kind=warning]`, `code[lang=python]`,
   `results[format=html]`, `strong`, `emphasis`, `link`, `cite[missing]`,
   plus *parts* (`box::label`, `box::title`, `list-item::marker`,
   `code::header`, `caption::label`) for generated pieces. Parents are part
   of the model (`slide > box > paragraph`), so selectors can use them.
   `mep-mepml tree FILE` dumps it as JSON so another editor can consume the
   structure without reimplementing the parser.
2. **Style** (`style.md`) — the `.mepss` language. CSS syntax (rules,
   declarations, comments, `@media`), deliberately a *small closed* property
   set that a cell-grid editor and a paged exporter can both honour.
3. **Rendering** (`rendering.md`) — what a conforming renderer must do with
   a computed style: the cascade order, inheritance, units, the fallback
   rules when a medium cannot express a property (an editor that cannot
   scale text must still colour it), and which behaviours are *not* style
   (cursor-row reveal, folding, running blocks — these are editor features).

### The style language (sketch)

```css
/* default.mepss -- what mep looks like today, written down */
heading            { font-weight: bold; }
heading[level=1]   { color: theme(OrgHeadlineLevel1, #1f2328); font-size: 1.6; }
heading[level=2]   { color: theme(OrgHeadlineLevel2, #1f2328); font-size: 1.32; }
strong             { font-weight: bold; }
verbatim           { color: theme(Green, #2f7d32); }
big                { font-size: 1.3; }
list-item::marker  { content: "•"; color: theme(Yellow, #9a6700); }
box                { background: fade(var(--accent), 0.09); border-left: var(--accent); }
box[kind=definition] { --accent: theme(Blue, #2c7fb8); label: "Definition"; }
box[kind=proof]    { end-mark: "∎"; }
callout[kind=warning] { --accent: theme(Yellow, #9a6700); }

@media present { heading[level=1] { font-size: 2.0; } }
@media html, pdf { box { background: tint(var(--accent)); } }
```

Decisions taken (flagged so they can be revisited):

- **CSS syntax, not mepml syntax**, for the style file. It is the syntax
  people already know for exactly this job, the HTML export can pass most
  of it straight through, and mep already has a CSS tokenizer in the HTML
  engine to lean on. Extension `.mepss`.
- **Colours are `theme(Group, #fallback)`** in the default sheet: the
  editor resolves the colour-scheme group (so the default look still
  follows the user's theme), exports use the literal. A plain `#rrggbb` or
  a mepml colour name is always allowed and means the same everywhere.
- **Sizes are unitless multiples of body text** (`1.3`), with `pt` accepted
  for exports. Spacing is in lines (`margin-top: 1`).
- **`@media` names are the existing export tags** (`editor`, `present`,
  `html`, `pdf`, `beamer`, `docx`, `slides` ...) — the same vocabulary
  `\when(...)` already uses, so one concept covers both.
- **Cascade**: built-in `default.mepss` → `~/.config/mep/mepml.mepss` →
  the document's `//? Style: file.mepss` (several allowed, in order; also
  inherited through `//? Import:`) → later rules win, then specificity as
  in CSS. No `!important`.
- **Generated text is style** (`label`, `content`, `end-mark`): "Definition",
  the bullet glyph, the tombstone. That is what lets a user rename or
  translate them. Numbering (`Figure 3`, `Slide 2`) stays structural; the
  sheet supplies the word around it (`label: "Figure"`).
- **Syntax stays out of the sheet.** Which `\name(` opens a box is still
  the parser's business; a sheet cannot add syntax (HTML's rule too).
  User-defined box kinds are a separate, later step (§ Step 10).

### What must not change

With no user sheet, every rendering — editor, presentation view, every
export — must be **identical** to today's. The default sheet is written to
reproduce the current constants exactly, and each step is verified against
goldens captured in Step 0 before any code moves.

## 3. Feature inventory (the "don't lose it" list)

Checked off against goldens at every step. Fixtures: `test.mepml`,
`tmp/regression.mepml`, `tmp/style.mepml`, `examples/mepl/iris_r/iris_r.mepml`,
`examples/mepl/slides/slides.mepml`.

Inline: bold, italic, underline, super/subscript, small, big, mono,
highlight, strike, insert, delete, verbatim, link (text|url, bare, #anchor),
`\f` family, `\fs` size, `\color`, nested combinations, footnote (number
replace), `\cite`/`\citep` labels (and the missing-key red), inline maths
(`$..$`, `\(..\)`, alttext), trailing `// comment`, escapes, `\raw`,
user-command calls.

Blocks: headings 1-6 (scale, slots, colour, indent), paragraphs, comments,
callouts (badge + sign-column bar, per keyword colour), document header
card (title/subtitle scale, key/value columns, Option colouring by type,
fold summary), `\import` links, citations/BibTeX, display maths (+alttext,
caption), code cards (lang chip, option chips, play button, title),
results cards (output / html / terminal / gui / markdown / web), figures
from `file=`, images, captions ("Figure N:" centred), alt text, tables
(grid, alignment, GFM, picture cells, inline maths width), lists (bullets,
numbers, checkboxes, nesting), rules, `\toc`, `\bibliography`, `\abstract`
(card + centred label), slides (card, "Slide N: title"), boxes (9 kinds:
tint, left rule, label, title, nesting, proof tombstone), `\define` /
`\when` / `\otherwise` / `\raw`.

Editor behaviours (not style, must keep working): cursor-row reveal,
conceal toggle, raw toggle, folds + fold summaries, patch-scan on cursor
move, soft-wrap slot arithmetic, presentation view (fill/full, autofit,
live blocks), running blocks, LSP, tree-sitter highlighting underneath.

Exports: html (document + slideshow), md, org, rtf, docx, odt, tex, pdf,
beamer, txt, pptx, odp; importers round-trip.

## 4. Steps

Each step ends with: build clean (`-Werror`, clang), the mepml test
binaries green, goldens compared, plan file updated.

### Step 0 — Baseline and safety net  `[x]`
- `tools/mepml_golden.py`: launches a throwaway mep under Xvfb per fixture,
  pages through the whole document (concealed, raw, cursor-on-row
  variants), saves PNGs; also runs every export through `mep-mepml convert`
  and stores the outputs. `--compare` diffs a second capture against the
  first (pixel-exact for the editor, byte-exact for text exports, unzipped
  XML for docx/odt/pptx/odp).
- Capture `baseline/` from the untouched tree. Record the pre-existing test
  failures so they are not mistaken for regressions.

### Step 1 — Specification, first draft  `[x]`
- `docs/mepml-spec/structure.md`, `style.md`, `rendering.md` as above,
  written *before* the code so the code implements the document.
- The full element table (name, attributes, parts, which `BlockKind` /
  `InlineKind` / `StyleFlag` it is today) and the full property table
  (name, values, inherited?, which media honour it).

### Step 2 — Style engine (pure, no editor)  `[x]`
- `src/mepml_style.{h,cpp}`: tokenizer, rules, selectors (type, `[attr]`,
  `[attr=v]`, `::part`, descendant, child, `*`, lists), `@media`, custom
  properties + `var()`, `theme()`, `fade()`, specificity + source order,
  inheritance, diagnostics with line/column. raylib-free like mepml_doc.
- `assets/mepml/default.mepss` embedded at build time.
- `mep-mepml-style-test`: parsing, cascade, every property, error
  recovery, a fuzz pass (sanitize build).

### Step 3 — Element tree  `[x]`
- `mepml::Elements(doc)`: the element for every block and inline, with
  parent links; `Span` gains its element + part so the editor never looks
  at `StyleFlag` to decide appearance again.
- `mep-mepml tree FILE` (JSON) and `mep-mepml style FILE [--media m]`
  (computed style per element) — the tooling another implementer needs,
  and what the tests assert against.

### Step 4 — Editor inline styling from computed styles  `[x]`
- `MepmlScan`: replace the colour chain, `SpanScale`, bold/italic/
  underline/strike policy, highlight background, marker glyphs and
  generated labels with lookups in the computed style.
- Goldens must be pixel-identical under the default sheet; then a test
  sheet that changes every inline property proves each one is live.

### Step 5 — Editor block styling  `[x]` (gaps in the log)
- Headings (scale/slots/colour/indent — split mepml from
  `kOrgHeadingStyles`, org keeps its own), callouts, header card, captions
  and alt text, rules, tables, list markers, `\toc`/`\bibliography`.
- Cards: `OrgBlockCard` carries a resolved style (wash colour + opacity,
  border, left rule, radius, chip colours) instead of `kind`/`tint`
  strings that main.cpp interprets; main.cpp's painter draws what it is
  told.

### Step 6 — Where sheets come from  `[~]`
- `//? Style:` header key (parser, grammar, LSP completion + diagnostics,
  missing-file error), user-global sheet, `:MepmlStyle` to open/reload,
  live reload when a sheet buffer is saved, sheet errors shown as
  diagnostics on the `//? Style:` line.
- `.mepss` filetype: highlighting, LSP-lite (property/element completion,
  unknown-property warnings) from the same tables the engine uses.

### Step 7 — Presentation view  `[x]`
- `@media present`: slide title sizes, page background, box look on
  slides; the autofit keeps working with user font sizes.

### Step 8 — Exports read the sheet  `[x]` (limits in the log)
- HTML: `kCss`/`BoxCss` become generated from the computed sheet (the
  `:root` custom properties stay, so existing user CSS keeps working).
- LaTeX/Beamer colours and tcolorbox styles; DOCX/ODT paragraph + character
  styles; PPTX/ODP deck styles; RTF. Each behind the same default-sheet
  byte-identity check, then a themed export checked by eye
  (chromium / LibreOffice / tectonic renders).

### Step 9 — Documentation and examples  `[x]`
- `help/mepml.org` chapter, `test.mepml` gains a styled section,
  `examples/mepml-styles/` with two or three complete themes (paper,
  dark deck, high-contrast) and a screenshot of each.
- Spec finalised against what was built; version it (`mepml-style 1`).

### Step 10 — Follow-ons (not required for the feature)  `[x]`
- User-declared box kinds (`\newbox(axiom)` in the document, styled from
  the sheet) so the 9 built-ins stop being special.
- Inline `\style(...)` blocks; per-element classes (`\class(name, text)`).
- Removing the leftover constants once nothing reads them.

## 5. Risks

- **Pixel identity** is the whole safety net; anything time-dependent in a
  screenshot (cursor blink, async maths) must be pinned in the harness.
- **Row metrics**: sizes change how many slots a row takes, and five row
  walkers must agree. User font sizes flow through the existing
  `mepml_row_scale` / heading-slot paths rather than new ones.
- **Scan cost**: the cascade runs per span. Computed styles are cached per
  (element signature, media) and invalidated with the sheet's generation,
  and the patch-scan fast path must stay a fast path
  (`just bench-mepml` before/after).
- **Theme coupling**: `theme()` keeps the default look tied to the colour
  scheme; a sheet with literal colours on a dark theme is the user's call.

## 6. Progress log

### 2026-10-02

**Step 0 done.** `tools/mepml_golden.py` (snapshot / capture / compare).
595 files per capture: 498 editor shots (5 fixtures, every 8 rows, plus
raw stops) and the exports. Findings that shaped it:
- the first capture must not be the baseline: its maths cache is cold and
  TeX source is on screen where a formula should be;
- a stop is settled only after three identical shots of the *pane* (the
  toolbar has an animated icon; the preview of the formula under the
  cursor arrives late) — two baseline runs then differ in ~1 shot of 498,
  always that preview popup;
- `mep-mepml-convert-test` fails at HEAD (test.mepml's missing
  test-gui PNG); the doc / lsp / ts tests pass.

**Step 1 done** (draft 1): `docs/mepml-spec/{README,structure,style,rendering}.md`.

**Step 2 done.** `src/mepml_style.{h,cpp}`, `assets/mepml/default.mepss`
(embedded by CMake as `mepml_default_style.h`), `mep-mepml-style-test`
(parser, error recovery, selectors, cascade, media, values, custom
properties, presentational markup, the default sheet, a 3000-round fuzz —
clean under ASan/UBSan). Beyond the first sketch: `:active`, `no-underline`
/ `no-line-through`, `#rrggbbaa`.

**Step 3 done** (tree only; the CLI dump is still to do).
`src/mepml_element.h`; `mepml::Highlight(doc, &paths, &block_nodes)` gives
every span its node (`Span::path`, and `Span::source_path` for a line
shown as source). Found on the way: `Highlight` read past a markdown
result block's text for its closing marker (heap overflow, masked by the
editor clamping the garbage column) — fixed.

**Step 4 done.** `Editor::MepmlScan` takes colour, weight, slant,
decoration, size, face, raise, background and generated text (list
markers, callout badges, box / abstract / slide labels, a proof's end
mark, the rule glyph) from computed styles. `ResolveHighlight` reads a
literal `#rrggbb` so a sheet colour goes wherever a group name did.

**Step 6, the core, done early** (Step 4 needed a way to load a sheet):
`//? Style: file.mepss` (several, in order; an import's come first),
`~/.config/mep/mepml.mepss`, and a sheet saved on disk restyles the open
documents within ~0.3 s. Still to do there: `.mepss` filetype and
language-server support, sheet errors as diagnostics.

**Step 5 done, bar the gaps listed below.** From the sheets now: cards
(`OrgBlockCard::look` — paper, title band, kind chip, outline, `:active`
outline, left rule — for header, abstract, box, slide, code, results);
heading size and the rows it takes (`Editor::HeadingStyleForRow`; org keeps
`kOrgHeadingStyles`); the header's title / subtitle / key labels / option
name and value; captions ("Figure 1:" is `caption::label`), alt text
(its 0.85 size too), `\toc` and `\bibliography` lines (`RenderedSpan::hl`,
set by `Editor::MepmlStyleRendered`); the callout's margin bar; and the
widths tables are laid out to (`ConcealedWidth` takes the computed size).
The hardcoded `CalloutColor`, `HeadingColor`, `SpanScale`, `SpanFamily`
and `kHeaderTitleScale` are gone.

**Step 3 finished**: `mep-mepml tree FILE` (JSON, `mepml::ElementTreeJson`)
and `mep-mepml style FILE [--media a,b] [--sheet S.mepss]`.

**Step 6 mostly done**: also the presentation view takes the document's
sheets (`MepmlPresentState::sheet_paths`, media `present`); the language
server reports a missing sheet and a sheet's errors on the `//? Style:`
line (`style-missing`, `style-error`) and completes the `Style` key;
`.mepss` files highlight through the CSS grammar. Not done: completion
and hover *inside* a `.mepss` file.

**Step 9 started**: `help/mepml.org` has a "Style sheets" chapter (html
regenerated, `check_help --strict` clean); `examples/mepml-styles/` has
`paper.mepss`, `contrast.mepss` and a document that uses them.
`test.mepml` is untouched (it is edited live): it still needs a
`//? Style:` line and a sheet beside it to stay "everything mepml can do".

**Gaps in the editor (still the renderer's own, not the sheet's):**
- a heading's `font-weight` / `font-style` / `text-align` on its scaled
  row (drawn by DrawPane's headline path; its colour and size are styled);
- a table's grid lines (the org grid pass draws them; `table::rule` only
  colours the pipes of a row shown as source);
- inside a code card: the option chips, the play / stop buttons, the title
  text; fold-summary bars; the `\toc` / `\bibliography` card's own paper;
- `margin-*`, `padding`, `border-radius`, `display` (specified as planned);
- generated text is limited to the glyphs the editor bakes
  (`kMathCodepoints`, main.cpp — the common dashes, bullets, squares,
  circles, arrows were added; anything else draws as `?`).

**Step 7** — see 2026-10-03 below.

### 2026-10-03

**Step 8 done for what each format can show cheaply; the rest listed.**
Design: an export keeps its built-in look (`kCss`, the LaTeX preamble, the
DOCX/ODT style tables) and applies only what the document's sheets
*change* — computed with and without them and compared — so no sheet
means byte-identical output (checked: all five fixtures, ten formats).
- `Document::sheets`, filled by `ParseForExport` (`mepml::LoadStyleSheets`:
  the user's sheet, then the `//? Style:` ones); `ExportMedia` adds
  `screen` / `print` to the export's tags.
- HTML and the slideshow: `mepml::ExportSheetCss` translates each rule —
  selector to the page's markup (`box[kind=proof]::label` →
  `.mbox-proof .mbox-label`; a deck's slide title is `heading[level=1]`),
  values to CSS (`theme()` → its fallback, `fade()` → `color-mix`, `var()`
  kept, a box's or callout's `--accent` also as the page's `--c`) — and
  appends it to the page's own CSS under `:root`.
- LaTeX / PDF / Beamer (generated from that HTML's markup, not its CSS):
  `HtmlWriter` wraps a run whose colour, weight or slant the sheets change
  in `<span class="mep-style" data-color data-bold data-italic>` and puts
  a box's colours and end mark on its `<div>`; `doc_export.cpp` reads
  both. Only when the export's tags include `tex`.
- DOCX, ODT, RTF, PPTX, ODP, Markdown, text: `mepml::ExportBoxLook`
  (label, accent, paper) replaces the `BoxKinds()` lookups.
- Tests: `TestExports` in `mep-mepml-style-test`. Seen rendered: HTML and
  HTML deck in chromium, PDF and Beamer through tectonic; DOCX / ODT / RTF
  by reading their XML (not opened in an office program).

Still the exports' own: text styling in the office formats (run colours,
heading colours and sizes), `font-size` / `font-family` / backgrounds in
LaTeX, a box's left rule being switched off in LaTeX, and anything under a
selector with ancestors (`slide bold`) in LaTeX, which sees an element
only under its block.

**Intended differences from the baseline** (default sheet, everything
else pixel-identical):
1. maths in a box's title is coloured like maths anywhere else on a line
   shown as source (it was forced to plain text);
2. a markdown result's `// result_end` line is muted like every other
   result marker (it took its colour from the out-of-bounds read above);
3. where two coloured inlines nest, the inner one's colour wins, except
   inside a link, an insertion, a deletion or a callout, which keep their
   own (it was a fixed priority order of kinds). No fixture has such a
   nesting.
4. `|mono|` text in a caption is the colour of mono text everywhere else
   (captions had a table of their own that made it green).

Verified by `tools/mepml_golden.py` against the baseline binaries: 595
files, every export byte-identical, every editor shot pixel-identical
except (1) and (2) and the harness's own ~1-in-500 preview-popup flake.

**Step 7 done.** A presented page was plain text with no idea it was a
slide: `slide heading` matched nothing and there was nowhere to hang a
background.
- `mepml::HighlightContext`: the view's page is highlighted inside
  `slide[number=N]`; the title page is `slide[number=0][title]` and its
  lines the header's `meta[key=title|subtitle|author]`
  (`Editor::MepmlPresentContext`).
- The slide's `background` is the page's paper (`Buffer::mepml_page_bg`,
  filled by DrawPane under the margins too); the in-pane erasures that
  painted the theme background (italic runs, table bands, card bands,
  the blank-until-maths-is-ready cover) now paint the page's.
- `default.mepss` has a `@media present` section that keeps the view as
  it was (no card, title at a heading's size, plain subtitle).
- HTML decks: `slide[title]` is `section.title-slide`, `meta[key=title]`
  its `<h1>`.
- The golden harness captures every page of the presentation view
  (`present-NNN.png`, 105 pages over three fixtures): two baseline runs
  identical, and the new build identical to the baseline.
- A sheet saved while presenting restyles the slide (checked).
Not checked here: full-screen mode (`<leader>kP`) — it needs a window
manager the capture display lacks; the font autofit it uses reads the
same row heights the sheets' sizes go through.

**Step 9 done.**
- The spec was read against the code and corrected: attributes the tree
  has but selectors do not see are marked †; `::label` / `::title` /
  `::end` list what actually carries them; the `content` defaults are
  tabled; `dark` / `light` media and lengths are marked planned or gone;
  rendering.md §7 lists where the editor stops short. It is versioned
  **mepml-style 1**, and `TestSpecification` (`mep-mepml-style-test`)
  fails if a property or element is named by the code and not the spec,
  or the other way round, or if `default.mepss` selects an unknown one.
- Three more hardcoded strings became `content`: a slide card's chip
  (`slide::label`), a folded box's chip (`box::label`) and the word before
  a caption's number (`caption[of=figure]::label { content: "Figure %n" }`).
  DrawPane no longer recognises a proof by the text "Proof"
  (`OrgBlockCard::end_mark`).
- `test.mepml` names `test.mepss` (`//? Style:`) and has a remark about
  it; the sheet shows an attribute selector, parts with `content`, a
  custom property and two `@media` sections. `mep-mepml-doc-test` checks
  the header line is parsed.
- `examples/mepml-styles/`: `deck.mepml` + `deck.mepss` (presenting), and
  `screenshots/` of the default look, `paper`, `contrast` and the deck.
- `help/mepml.org`: generated text, the presentation view, what each
  export takes.
- Harness: a capture now starts from a pristine copy of the fixtures
  (`fixtures.pristine`) — presenting a deck starts its live web blocks,
  which rewrote two of the fixture's own pictures and showed up as export
  and editor differences that were not the code's.

**Step 10 done.**
- **Boxes of any kind**: `\boxed(kind, Title,` ... `)` — the same
  `box[kind=...]` element as the nine built-in commands, labelled with its
  kind until a sheet names it (`mepml::BoxLabel`: `key-result` → "Key
  result") and drawn as a plain box until a sheet colours it. Not `\box`:
  the tests, the benchmark and the grammar's own corpus all `\define` a
  user command called `box`, and a built-in name would have broken every
  document that does. (This replaces the planned `\newbox` declaration:
  no declaration means nothing for the parser, the grammar or an import to
  resolve first.) Markdown, Org (`#+begin_box_axiom`) and HTML bring a
  kind back; the office formats write its heading in the default box
  style, as they do for the built-in kinds.
- **Classes**: `\class(name, text)` is `span[class=name]`, selected as
  `.name` (`[class=name]` for short, on any element). HTML writes
  `<span class="mep-name">` and the sheet's `.name` rules as `.mep-name`;
  LaTeX takes its colour, weight and slant; DOCX and ODT write a character
  style per class (`mep class name`) carrying that look, RTF the style's
  name alone — and all read it back (`InlineKind::Class`, round trips in
  `mep-mepml-convert-test`).
- **A sheet in the document**: `\raw(style, rules ...)`. `style` is a
  format no export has, so the existing `\raw` machinery keeps the block
  out of every export; its text is a sheet applied after the header's
  (`mepml::InlineStyleSheets`: editor, presentation view, exports, and
  `mep-mepml style`). The language server reports its errors on their own
  lines. (Instead of a new `\style(` directive: no new syntax.)
- **Leftover constants removed**: `BoxKind::hl` and `OrgBlockCard::tint`
  (the editor's hardcoded box colours), with DrawPane's fallbacks for
  them. `BoxKind::color` / `tint` stay as the exports' built-in look.
- Grammar: `boxed` is a box kind, `class` a rule beside `color`
  (`_cmd_class` external, added last); parser regenerated, corpus 42/42,
  `mep-mepml-ts-test` agrees on `test.mepml`, which now has all three.
- Spec: `span`, `.name`, kinds of the document's own, cascade step 4.

**After the plan: the open items.**
- *Office exports' text.* `mepml::ExportTextStyler` (the change-vs-default
  logic the HTML writer had for LaTeX, now shared). DOCX and ODT carry a
  heading's, paragraph's or inline's sheet colour / weight / slant as a
  character style named for the change (`MepSheet-cAA0000-b1`), which
  their readers ignore — writing it on the run itself failed the round
  trip at once (sheet-bold text came back as `*bold*`). PPTX / ODP set it
  on the runs (nothing reads those back). RTF gets none: it has only the
  run to put a look on.
- *Editor gaps.* Headings draw their `font-weight`, `font-style` and
  `text-align` on the scaled row (`Buffer::mepml_heading_look`); tables
  take `background` (stripe hue), `border-color`, `:active` and
  `table::rule` (`OrgTableGrid::look` / `rule`); the `\toc` /
  `\bibliography` card takes `background` and `border-color`; a code
  card's chip text, title and option chips take `code::label`'s `color`,
  `code::title` and `code::option`. (`text-align` on ordinary text, the
  card buttons and arbitrary glyphs: done next, below.)
- *`.mepss` files.* `mep-mepml-lsp` answers for them too
  (`MepssLspDiagnostics` / `Completions` / `Hover`; the server tells a
  sheet by its name, the editor registers the `mepss` filetype): parse
  errors, unknown elements and parts with a suggestion, and completion by
  context — selector, `[attr`, `[attr=`, `::`, `:`, property, value,
  `theme(`, `@media`.


**After the plan: the renderer's own items** (the editor gaps left above).
- *`text-align` on running text.* The scan records the alignment of each
  row of a `paragraph`, `list`, `callout` or `abstract`
  (`Buffer::mepml_row_align`); `DrawPane` moves `text_x` for that row once
  its drawn line is known, so text, spans, spell marks and selection move
  together, by whole cells within the text width. Not the caret's row
  (drawn where it is typed), not a row that takes more than one visual
  line; `justify` is `left`. The line number and the quick-jump wash keep
  the unshifted origin (`text_x_left`).
- *A heading's `text-decoration`.* `MepmlHeadingLook::underline / strike /
  line_hl`, drawn as rectangles on the scaled row; the heading's recolor
  pass skips underline / strike decorations, whose colour is the line's,
  not the text's.
- *Code card.* `code::button`'s `color` is the run / stop / fold
  controls' (`OrgCardLook::button`), `code::header`'s `color` the option
  chips' values (`header_text`). The default sheet sets neither, so the
  built-in look is unchanged.
- *Glyphs.* `Editor::MepmlGlyphs()` collects the non-ASCII characters of
  every computed `content`; the frame loop adds those JetBrains Mono has
  (cmap lookup, `gfx::tt::FindGlyphIndex`) to `g_math_extra_codepoints`
  and re-bakes the math face (`BakeMathFont`). One the face lacks still
  shows as `?`.
- *Word / Writer.* A sheet's colour for inserted / deleted text replaces
  the built-in one (`SheetLook::TakeFor`); the character style is tagged
  `MepSheet-ins-…` / `-del-…` so the readers still know the run for what
  it is (`Fmt::sheet_tag`). `\color` text keeps the author's colour: that
  is the document's, not the sheet's.
- rendering.md §7 now lists only what the character grid bounds.
