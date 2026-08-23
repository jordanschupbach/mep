# WebKit Parity Plan

Living roadmap for growing mep's in-house HTML/CSS/JS browser pane
(`src/html_doc.h/.cpp`, `src/js_engine.h/.cpp`, `Mode::Html`/`HtmlSession`
in `editor.h/.cpp`, the layout/render pass in `main.cpp`) from "renders a
static hand-written page with a toy script" toward genuine WebKit-class
capability: full CSS, a real JS engine, and enough DOM/BOM surface to run
real interactive web apps — including JS frameworks like React — inside a
mep pane. Written so a new session can pick this up cold: read **Scale
check** and **What mep's browser already has** first, then **Status** for
where things stand, then the relevant phase for what's left in it.

**How to resume:** check the checkboxes below, `git log --oneline` for
what's landed, then continue with the first unchecked item in the
lowest-numbered incomplete phase. Phases are ordered so each one only
depends on earlier ones — Part I deepens the rendering model everything
downstream assumes; Part II gives Part III's JS-driven interactivity an
engine capable of running it; Part III is the one that actually unlocks
"run a JS framework," and is this plan's real target, not an afterthought
tacked onto the end. Follow the same rigor `NVIM_PARITY_PLAN.md` and
`VIM_PARITY_PLAN.md` established: implement → build `build/native` →
verify under Xvfb+xdotool (screenshot the actual rendered page, don't just
trust that it compiled) → update this file's checkboxes with a short
"Verified via..." note → move to the next phase, pausing for the user's
go-ahead unless told to keep going.

---

## Scale check, read this first

**"Full parity with WebKit" is not a project with an end date.** WebKit
itself is on the order of 10+ million lines of code, built by hundreds of
engineers over 20+ years, and still doesn't render every page on the live
web pixel-perfectly. Nothing in this plan pretends a from-scratch,
mostly-one-engineer-at-a-time effort closes that gap. What it *does* aim
for is real, honestly, in three calibrated tiers:

1. **Parts I–IV are the actually-achievable core**: a browser engine that
   correctly lays out real-world HTML+CSS (box model, flexbox, a real
   selector engine), runs real modern JavaScript (proper prototypes,
   async/await, a real event loop), and exposes enough DOM/BOM API
   surface that an unmodified, webpack/vite-bundled JS app — React
   included — can mount, render, and respond to clicks. **Part III Phase
   12 ("run a real React app") is this plan's actual finish line for the
   thing the user asked for.** Everything before it exists to make that
   phase possible; everything after it is fidelity/breadth/performance
   polish on top of a working foundation.
2. **Parts V–VI are real, worthwhile, and open-ended-but-tractable**:
   visual fidelity (grid, transforms, animations, pseudo-elements) and
   performance (a bytecode VM instead of a tree-walking interpreter,
   incremental layout). Each phase here has a clear "done enough" bar —
   ship the 80% that covers real pages, don't chase the last 20% of any
   one CSS property's edge cases.
3. **Part VII is the genuine long tail**: Canvas/WebGL/SVG/audio-video
   codecs/Web Components/service workers/accessibility/i18n. This is
   where "full WebKit parity" as a literal goal lives, and it is
   realistically unbounded — real browser vendors have entire teams per
   item in that list. Part VII is deliberately left at survey depth, not
   phase-by-phase engineering detail, precisely because writing detailed
   sub-phases for e.g. WebGL2 conformance would be performative rather
   than useful this far out. Pick off individual Part VII items later,
   driven by "a real page/app I want to view hits a wall here" rather
   than working the list in order.

Treat every phase below as its own multi-session project, the same way
`NVIM_PARITY_PLAN.md` treats its own phases. It's completely reasonable to
implement a fraction of a phase's scope (e.g. flexbox's `row`/`column` +
`justify-content`/`align-items` without `flex-wrap` or `order`) and mark
it "done enough" with a note, matching how `VIM_PARITY_PLAN.md` and
`NVIM_PARITY_PLAN.md` both treat stretch goals. Prefer a smaller, solid,
well-tested slice over a large, half-working one — this matters *more*
here than in most of this codebase's other parity plans, because a
half-implemented CSS property or JS operator doesn't just look wrong, it
can silently make real-world pages/apps fail in confusing ways.

---

## What mep's browser pane already has (the Part 0 baseline)

Don't re-derive any of this — it's the foundation every phase below
builds on:

- **HTML parsing** (`html_doc.cpp`'s `ParseHtml`): a hand-rolled
  tokenizer + stack-based tree builder. Handles nested tags, attributes,
  void elements, `<script>`/`<style>`/`<textarea>` raw-text bodies,
  comments, `<!DOCTYPE>`, a common-entity subset (`DecodeEntities`), and
  tolerates malformed markup (auto-closes at the nearest matching
  ancestor, treats unknown tags as generic containers) the same way
  `PdfDoc`/`LoadDocxFromMemory` tolerate malformed input elsewhere in
  this codebase. Not a real HTML5 tokenizer state machine (no tree-
  construction "insertion mode" table, no encoding sniffing) — see Phase
  1's own scope.
- **CSS** (`html_doc.cpp`'s `ComputeStyles`/`ComputedStyle`): a UA
  per-tag default stylesheet, `<style>` block parsing with tag/`.class`/
  `#id` selectors only (no combinators, no specificity beyond a fixed
  tag-then-class/id two-pass order), inline `style=""`, and inheritance
  for color/bold/italic/underline/strikethrough/monospace/
  preserve_whitespace. No box model at all yet — `margin_top_lines`/
  `margin_bottom_lines` are the *only* spacing concept, in text lines,
  not real margin/border/padding/width/height boxes.
- **Layout** (`main.cpp`'s `HtmlLayoutBlock`/`HtmlCollectInlineWords`/
  `HtmlFlushWords`): block elements stack vertically; inline content
  word-wraps within the pane width using real font metrics
  (`MeasureTextEx`); `<pre>` preserves whitespace verbatim; `<li>` gets a
  bullet/numbered marker and indent. Recomputed fresh every `DrawPane`
  call rather than cached (see `HtmlSession`'s own comment, `editor.h`) —
  fine for hand-written pages, a real perf problem once Part VI's real
  apps are in scope.
- **Rendering** (`main.cpp`'s `DrawHtmlRun` and `DrawPane`'s html
  branch): one monospace font (`g_font`) drawn at varying sizes for
  headings; bold faked as a 1px-offset double-draw, italic faked as an
  `rlgl` shear transform (same techniques `main.cpp`'s org-emphasis
  decoration renderer already uses for the main text buffer) — no real
  bold/italic font faces, no proportional font at all yet.
- **JS engine** (`js_engine.cpp`): a tree-walking interpreter — `var`/
  `let`/`const` (unified, no block scoping), function declarations/
  expressions, arrow functions, `if`/`else`/`while`/`for`/`return`/
  `break`/`continue`, all standard operators including `++`/`--`,
  template literals, array/object literals, a statement-count + call-
  depth safety guard against infinite loops/recursion. Real closures,
  real (if simplified) scope chains. No classes, no generators, no
  `async`/`await`/`Promise`, no prototype chain beyond what's needed for
  the DOM binding objects, no `Map`/`Set`/`RegExp`/`JSON`/most of
  `Array.prototype`, no garbage collector (each `RunScripts` call is a
  bounded, one-shot execution today, so plain C++ RAII cleanup is
  sufficient — Part II Phase 8 is where this stops being true).
- **DOM binding surface**: exactly four things —
  `document.getElementById`, `document.title` get/set, an element's
  `.textContent` get/set, `console.log`. That's the entire API a script
  can touch. Everything in Part III Phase 10/11 is net-new.
- **Script execution model**: every `<script>` block runs exactly once,
  synchronously, immediately after the DOM finishes parsing, in one
  shared global scope. No event loop, no re-entry — a script that
  registers a `setTimeout`/click handler today has literally nowhere for
  that callback to ever run. Part III Phase 9 is what fixes this.
- **Pane integration** (`editor.h`'s `HtmlSession`/`Mode::Html`,
  `editor.cpp`'s `HandleHtmlInput`/`OpenHtmlInPlace`): a real mep pane —
  own buffer/session, own scroll (`j`/`k`/`Ctrl-d`/`Ctrl-u`)/zoom (`+`/
  `-`/`=`), own header label — following the exact same
  `ImageSession`/`PdfSession` pattern the rest of this codebase's viewer
  panes use. `:e foo.html` still opens plain editable text; only
  `:Browse`/`mep.html_open` opens the rendered pane. This split stays
  exactly as-is throughout this whole plan — nothing here changes how
  the pane is *reached*, only what it can *render/run* once open.
- **Networking**: `mep.browse_command` (`kBuiltinTextTools`, `main.cpp`)
  `curl`-fetches a `http(s)://` target to a fresh temp file before
  opening it — the *only* network I/O anywhere in this feature today.
  No subresource fetching (a page's own `<link>`/`<script src>`/`<img
  src>` are never fetched), no fetch()/XHR from JS.
- **The WebKit escape hatch stays**: `mep.browse()`/`:BrowseExternal`/
  `<leader>bO` still open a page in a real WebKit window
  (`launcher/browser.ts`, `jsr:@webview/webview`) for anything the
  in-house engine can't yet handle. This plan does not remove that path
  — it's the pressure-relief valve for real-world pages while the
  in-house engine is still catching up, and arguably never needs to go
  away even once Part III lands.

---

## Design decisions

- **The JS engine grows in place; there is no plan to vendor a real
  engine (V8/JSC/QuickJS/Hermes) instead.** This was an explicit choice
  when the feature was first built (the user asked for something "built
  inhouse," not an embedded third-party engine) and it holds for the
  rest of this plan too. It means Parts II/VI are genuinely large
  undertakings — building a spec-compliant JS engine (even a slow one)
  is most of what a browser vendor's JS team does full-time — but it's
  the whole point of the exercise, and it keeps the dependency graph
  exactly what it's been throughout this codebase: no new external
  library, just more C++ in `src/`.
- **Fonts: bundle a real proportional face (and real bold/italic weights
  of it), the same way `font_data.h`/`icon_font_data.h` already embed
  JetBrains Mono and the Nerd Font icon subset.** Phase 4 is where this
  lands. Until then, monospace-at-varying-sizes with faked bold/italic
  is an accepted, ugly-but-functional placeholder — don't let font
  fidelity block earlier phases that are really about layout/JS
  correctness.
- **No GPU compositing, no layer tree, no scroll-as-a-separate-thread.**
  Everything renders through raylib's immediate-mode draw calls the same
  way the rest of mep does, recomputing layout each frame (Part VI Phase
  21 adds *incremental* recomputation, not a compositor). This is a
  meaningful, permanent gap versus real WebKit's rendering architecture,
  and it's fine — mep is a text editor with a browser pane bolted on,
  not a standalone browser competing on scrolling smoothness.
- **Networking stays curl-subprocess-based, not a hand-rolled HTTP/TLS
  stack.** `mep.job_start`-spawned `curl` calls (Part IV) reuse exactly
  the mechanism `mep.browse_command` already uses for the top-level page
  fetch. Writing an HTTP client (let alone TLS) from scratch would be a
  parity plan of its own and buys nothing curl doesn't already do
  correctly. `fetch()`/XHR (Phase 14) become async wrappers around the
  same subprocess primitive, not a new networking layer.
- **Part III Phase 12 ("run a real React app") is the plan's actual
  target, called out explicitly as a milestone rather than left implicit
  at the end of a phase list.** Everything in Parts I–III is sequenced
  specifically to make that milestone reachable as early as honestly
  possible, even though visual fidelity (Part V) and performance (Part
  VI) will both still be rough at that point. Don't let "but the CSS
  isn't pixel-perfect yet" block attempting Phase 12 once Phases 1–11
  are in reasonable shape — the milestone is "mounts and responds to a
  click," not "looks identical to Chrome."
- **CSS selector/cascade correctness (Phase 2) comes before deep box-
  model work (Phase 1 still ships first, since layout is meaningless
  without *some* box model) because most real pages/frameworks lean on
  selectors far more than mep's current two-bucket tag/class approximation
  can handle** — descendant combinators (`.foo .bar`), attribute
  selectors (`[type="text"]`), and `:hover`/`:focus`/`:nth-child` are
  used pervasively by real CSS (including whatever a CSS-in-JS or
  Tailwind-style toolchain emits for a React app), not edge cases.
- **The event loop (Part III Phase 9) is the single most
  architecturally significant change in this whole plan** — it turns
  the HTML pane from "parse once, run scripts once, render passively"
  into "a live process that keeps running JS across frames, dispatches
  input as DOM events, and re-renders on mutation," i.e. an actual
  application host instead of a static document viewer. Budget real
  time for this one; nearly everything in Part III depends on it working
  correctly, and it touches mep's own per-frame update loop
  (`main.cpp`'s render loop / `Editor::HandleInput`), not just
  `js_engine.cpp`.
- **Security posture**: page JS has never had, and must never gain,
  access to `mep.*`'s own Lua API, the filesystem, or subprocess
  spawning beyond what Phase 14's fetch()/XHR explicitly allows (and
  even that should go through the same curl-subprocess sandboxing, not
  a raw socket API handed to the page). This should be treated as a
  standing constraint checked at every phase from Part III onward, not
  a one-time Phase 30 audit — see Part VII's security phase for the
  belated formal pass, but don't wait for it to start being careful.

---

## Status

Two slices have landed out of band, ahead of Part I proper, driven by a
concrete real-world need rather than phase order: **local `<img>`
rendering** and **inline/display LaTeX math rendering**, specifically so
org-mode's default HTML export (MathJax-delimited `\(..\)`/`\[..\]` math
plus `<img>` figures) renders as a real equation/image instead of raw
LaTeX source text or a bracketed placeholder. See "Addendum" just below
for what shipped, what it deliberately doesn't cover, and where it fits
against the phase list. Everything else in this plan is still
unimplemented — Phase 1 (full CSS box model) is next up in Part I proper.

- [ ] Part I — Rendering-model depth (CSS box model, selectors, more
      elements, font fidelity) — **local `<img>` rendering landed early
      (see Addendum); box model/selectors/tables/forms/proportional fonts
      still open**
- [ ] Part II — A real JS engine (syntax, prototypes/built-ins, async,
      GC)
- [ ] Part III — Live application host (event loop, DOM mutation,
      events) — **Phase 12 milestone: run a real React app**
- [ ] Part IV — Networking & resource loading
- [ ] Part V — Visual fidelity (flexbox, grid, transforms, pseudo-
      elements)
- [ ] Part VI — Performance (bytecode VM, incremental layout)
- [ ] Part VII — The long tail (canvas/SVG/WebGL/audio-video/web
      components/service workers/a11y/i18n) — open-ended, survey depth
      only

---

## Addendum: local `<img>` + LaTeX math rendering (landed 2026-08-21)

Implemented ahead of the phase order above because org-mode's default
`ox-html` export is a concrete, common target for this browser pane, and
its two defining features -- inline figures and MathJax-delimited math --
were the two most visible gaps in the Phase 0 baseline (images: an
explicit, documented placeholder; math: not mentioned anywhere in this
plan at all, a real oversight -- MathJax's `\(..\)`/`\[..\]`/`$..$`/`$$..$$`
spans are plain text in the DOM, not a CSS/DOM feature, so they didn't
naturally fall under any Part I/II/III phase as originally scoped).

- **Local `<img>` rendering** (`main.cpp`'s `HtmlCollectInlineChild`):
  resolves `src` against the open file's own directory (`HtmlLayoutCtx::
  base_dir`, from `HtmlSession::source` -- `l_html_open`, lua_env.cpp, now
  absolute-izes `source` specifically so this resolution survives a later
  `mep.fs_chdir`), loads+GPU-uploads via the same mtime-cached
  `GetOrLoadOrgInlineImageTexture` org's own inline-image links already
  used, and lays the image out inline sized to its `width`/`height`
  attrs (if given) or natural size, clamped to the pane's content width
  and scaled by the pane's own zoom. A **real pre-existing bug** was
  found and fixed in the process: an `<img>` that's a *direct* block-level
  child (`<p><img src=...></p>`, exactly org's own figure-export shape --
  see `demo.org`'s own test case, described below) rendered nothing at
  all, not even the placeholder text, because `HtmlLayoutBlock`'s per-
  child dispatch called `HtmlCollectInlineWords` on the `<img>` node
  itself rather than on the container that held it, so it iterated the
  (empty, void-element) `<img>`'s own children instead of ever inspecting
  the `<img>` node. Fixed by refactoring the whole inline-collection path
  into one recursive `HtmlCollectInlineChild(child, parent_style, ctx,
  out)`, called uniformly whether the leaf is nested in a container
  (`<span><img></span>`) or a direct block child. **Still out of scope**:
  remote (`http(s)://`) `<img src>` -- no subresource fetching yet (Part
  IV Phase 13 is where that lands); falls back to the bracketed
  placeholder exactly as before.
- **LaTeX math rendering** (`html_doc.cpp`'s `ExtractMathSpans` +
  `main.cpp`'s new mini math typesetter): a parse-time pass splits
  `\(..\)`/`\[..\]`/`$..$`/`$$..$$` spans out of Text nodes into synthetic
  `<math display="0|1">` DOM elements (display = block/centered, inline =
  flows with surrounding text), skipping `<script>`/`<style>`/`<pre>`/
  `<code>` subtrees the same way MathJax's own default config does. A
  from-scratch recursive-descent parser + layout engine
  (`MathNode`/`MathParser`/`LayoutMathExpression`, `main.cpp`) handles
  superscript/subscript (including nesting), `\frac`, `\sqrt` (overline +
  radical, not a real stretching radical sign), `\text`/`\mathrm`,
  `\left`/`\right` (sizing hint dropped, delimiter rendered plain), and a
  ~70-entry symbol table (Greek letters, common operators/relations/
  arrows/set-theory symbols) -- verified end-to-end against a *real*
  `emacs --batch -f org-html-export-to-html` export (not hand-written
  HTML) rendering the quadratic formula, Euler's identity, and a
  summation correctly, screenshotted under Xvfb. A new `g_math_font`
  atlas (same embedded JetBrains Mono TTF as `g_font`, baked with a wider
  explicit codepoint list -- ASCII + Greek + the symbol table's Unicode
  points) was added specifically for this, kept separate from `g_font`'s
  own bake so the hot per-character glyph-index cache
  (`g_glyph_index`/`CacheGlyphIndices`) stays untouched. **Still out of
  scope, real limitations, not just polish**: `\begin{equation}`/other
  LaTeX environments, matrices/arrays, `\newcommand` macro definitions,
  real radical-stretching (the sqrt sign doesn't grow to the radicand's
  actual height, just draws a same-size glyph plus an overline), a real
  TeX kerning/spacing model (inter-symbol spacing is a flat heuristic, not
  TeX's actual italic-correction math), and an unmapped `\command` shows
  its own name as plain upright text rather than failing -- legible but
  not pretty. Real MathJax config scripts (the `<script id="MathJax-
  script">` bootstrap `ox-html` also emits) now execute without erroring
  too, incidentally: `js_engine.cpp`'s global scope gained a bare, inert
  `window` object (property-assignment only, no BOM methods) specifically
  so the extremely common `window.Foo = {...}` config-stashing pattern
  doesn't throw `ReferenceError` -- a small, generically useful addition
  to the DOM/BOM binding surface, not math-specific.
- **Where this leaves Phase 1/3**: the box model (Phase 1) and a real
  selector engine (Phase 2) are both still fully unimplemented -- image
  sizing above uses only the raw `width`/`height` HTML attributes, no CSS
  `width`/`max-width`/etc. Math rendering has no equivalent "Phase" in the
  list above at all; if it grows further (more LaTeX commands, matrices,
  a real radical), track it as its own addition here rather than
  shoehorning it into a Part I/II CSS/JS phase it doesn't really belong to.

---

## Part I — Rendering-model depth

### Phase 1 — Real CSS box model

Today's `ComputedStyle` has exactly one spacing concept
(`margin_top_lines`/`margin_bottom_lines`, in text lines). Real CSS needs
the actual box model: content box, padding, border, margin, each
independently settable per side, plus `box-sizing` (`content-box` vs
`border-box`) and explicit `width`/`height`/`min-*`/`max-*`.

- [ ] Extend `ComputedStyle` (`html_doc.h`) with per-side
      `margin`/`padding`/`border_width` (px, not lines) and
      `border_color`/`border_style` (solid only for v1 — dashed/dotted/
      double are Phase 19 polish), plus optional `width`/`height`
      (unset = auto-size to content, same as today).
- [ ] Extend `ApplyDeclarations` (`html_doc.cpp`) to parse `margin`/
      `padding`/`border`/`width`/`height` and their per-side
      longhand/shorthand forms (`margin-top`, `margin: 1px 2px`, etc.),
      plus `box-sizing`.
- [ ] Rewrite `HtmlLayoutBlock` (`main.cpp`) to compute real box
      geometry per node — content rect inset by padding+border from the
      border rect, itself inset by margin from the node's allocated
      space — instead of the current "just advance `cursor_y` by a line-
      count margin" model. This is the single biggest layout rewrite in
      Part I; get it right here since Phases 16/17 (flexbox/grid) build
      directly on whatever box-geometry primitive this phase produces.
      Adjacent-margin collapsing (a real CSS quirk: two stacked block
      elements' vertical margins merge into the larger one, not sum) is
      worth getting right too — it's surprisingly load-bearing for
      "does spacing look like a real browser's."
- [ ] Draw `border`/background per the real box geometry (`DrawPane`'s
      html branch) — a background-color rect currently isn't drawn at
      all per-element, only implicitly via the pane's own flat
      background.
- [ ] CSS units beyond bare numbers: `px` (already assumed), `%` (of
      the containing block), `em`/`rem` (already partially done for
      `font-size`; extend to margin/padding/width), viewport units
      (`vw`/`vh`) are lower priority — real pages use them but a pane
      isn't really a "viewport" the way a window is; punt if not cheap.
- [ ] Verify against a hand-written test page with nested bordered/
      padded boxes, `box-sizing: border-box` vs default, and a page with
      several stacked `<p>`s with different margins (checks collapsing).

### Phase 2 — CSS selector engine + cascade

Today: tag selectors and `.class`/`#id` selectors, exact match only, two
fixed passes (tag rules always lose to class/id rules) — no combinators,
no attribute selectors, no pseudo-classes, no real specificity
calculation.

- [ ] Selector parser (`html_doc.cpp`) covering: descendant (`a b`),
      child (`a > b`), adjacent/general sibling (`a + b`, `a ~ b`),
      attribute selectors (`[attr]`, `[attr="val"]`, `[attr~="val"]`),
      comma-separated selector lists (already partially handled),
      multiple classes on one selector (`.a.b`).
- [ ] Real specificity calculation (the standard (a,b,c,d) tuple: inline
      style > id count > class/attribute/pseudo-class count > tag/
      pseudo-element count) replacing the current two-bucket
      approximation, with source order as the tiebreaker within equal
      specificity — this is what makes cascade behavior match real
      browsers instead of "close enough for a hand-written test page."
- [ ] Structural pseudo-classes: `:first-child`, `:last-child`,
      `:nth-child(n)`, `:nth-of-type(n)` — computable purely from DOM
      structure, no interaction state needed.
- [ ] Interaction pseudo-classes: `:hover`, `:focus`, `:active` — these
      need to know mep's own live mouse position / whichever element has
      "focus" within the page, which doesn't exist as a concept yet
      (Part III Phase 11's event system is the natural place this
      becomes real; a `:hover` that never activates is an acceptable
      stub until then — note the dependency rather than half-building
      focus tracking twice).
- [ ] `document.querySelector`/`querySelectorAll` (`js_engine.cpp`) once
      the selector engine above exists — this is the single most-used
      DOM API in real-world JS, including inside React's own internals
      and virtually every non-framework script, so it belongs early
      even though it's technically a Part III (DOM API) concern; land it
      here once the matcher exists rather than rebuilding selector
      matching twice.
- [ ] Verify against a page using descendant/child selectors, an
      attribute selector, and `:nth-child` for zebra-striping a list.

### Phase 3 — More elements, real tables, basic forms

- [ ] Table layout: `<table>`/`<tr>`/`<td>`/`<th>`/`<thead>`/`<tbody>`/
      `<tfoot>` as real row/column grid layout (measure each column's
      widest cell content, lay out cells accordingly) — today they're
      just generic block/inline containers with no actual grid.
      `colspan`/`rowspan` are real-world-common enough to include; full
      `border-collapse` semantics are lower priority.
- [ ] Form elements as real interactive widgets, not inert text:
      `<input type="text">`/`type="checkbox"`/`type="radio"`, `<button>`,
      `<select>`/`<option>`, `<textarea>`. This needs actual widget state
      (a text input's current value + cursor position, a checkbox's
      checked state) living somewhere — natural home is a new field on
      `DomNode` or a side-table keyed by node pointer, mirroring how
      `HtmlSession` itself is a side-table keyed by buffer id. Rendering
      reuses mep's own text-cursor-drawing conventions where sensible.
      This phase can ship read-only-looking widgets (render the current
      state, no keyboard/mouse interaction) before Part III Phase 11's
      event system exists to make them actually editable — note that
      dependency rather than blocking on it.
- [ ] Remaining common elements currently falling through as generic
      containers: `<figure>`/`<figcaption>`, `<details>`/`<summary>`
      (needs an open/closed toggle state, same widget-state mechanism as
      forms above), `<dl>`/`<dt>`/`<dd>`, `<blockquote>` (already
      styled, just double-check indentation), `<iframe>` (out of scope
      entirely for now — nested browsing contexts are a Part VII-class
      problem; render a placeholder).
- [ ] Verify against a page with a real data table and a small form
      (text input + checkbox + submit button, no working submission yet
      — that's Part III/IV territory).

### Phase 4 — Text/font fidelity

- [ ] Embed a real proportional font family (regular/bold/italic/bold-
      italic, at minimum a sans-serif — a serif face too if cheap) the
      same way `font_data.h` embeds JetBrains Mono: license-compatible
      (OFL, matching the existing fonts), subsetted if the full family
      is large (mirrors `icon_font_data.h`'s `pyftsubset` approach, though
      a body-text font needs a much broader glyph/Latin-coverage subset
      than the ~59-glyph icon set — don't over-subset and start clipping
      real page text).
  - [ ] `font-family` CSS property resolution: a small generic-family
        fallback chain (`sans-serif`/`serif`/`monospace` map to the
        bundled faces; an unrecognized named font falls back to
        sans-serif) — real `@font-face`/web-font loading is Part IV/VII
        territory (needs fetching), not this phase.
  - [ ] Real bold/italic/bold-italic *faces* replace the faked double-
        draw/shear from Phase 0's baseline wherever the new proportional
        font is in use; keep the fake-bold/shear-italic fallback for
        monospace/`<pre>`/`<code>` content, which stays on the
        monospace face on purpose (matches every real browser's own
        `<pre>` treatment).
- [ ] `text-align` (left/center/right/justify), `line-height`,
      `letter-spacing`, `white-space` (`normal`/`pre`/`nowrap` — today
      only `<pre>`'s implicit `pre` behavior exists).
- [ ] Proportional-font word-wrap: `HtmlFlushWords`'s wrap math already
      measures per-word via `MeasureTextEx`, which already works
      correctly for a non-monospace font (it was never hardcoded to
      assume fixed character width) — this should mostly "just work"
      once a proportional `Font` is plugged in as an option per
      `HtmlLayoutCtx`; verify it actually does rather than assuming.
- [ ] Verify visually: the same test page from earlier phases should go
      from "looks like a manpage" to "looks like a real (if plain) web
      page" — this phase is the one most worth an actual side-by-side
      screenshot comparison against a real browser's rendering of the
      same minimal page.

---

## Part II — A real JS engine

Everything here lives in `js_engine.cpp`. The guiding principle: grow
toward real ECMAScript semantics, not toward "whatever gets today's test
page to pass" — Part III's React milestone will exercise language
features far beyond what any hand-written test script does, and
retrofitting semantic correctness (prototype chains especially) after
Part III code depends on the sloppy version is much more expensive than
getting it right here.

### Phase 5 — Full modern syntax

- [ ] Classes: `class`/`extends`/`super`/constructor/methods/static
      members/getters&setters/private fields (`#x`) — React class
      components (`class Foo extends React.Component`) are less common
      in new code (hooks-based function components dominate) but still
      extremely common in real-world code, and the class syntax
      underpins a lot of non-React JS too.
- [ ] Destructuring (array and object, including nested and default
      values) in variable declarations, function parameters, and
      assignment targets.
- [ ] Spread/rest (`...`) in array/object literals, function calls, and
      function parameters.
- [ ] Default parameters, computed property names (`{[key]: val}`),
      shorthand object properties (`{x, y}`), method shorthand
      (`{foo() {}}`).
- [ ] `for...of` / `for...in`, labeled statements + labeled
      `break`/`continue`, `switch`/`case`.
- [ ] Optional chaining (`?.`) and nullish coalescing (`??`) — extremely
      common in real modern JS, including React/library internals.
- [ ] Generators (`function*`/`yield`) — lower priority than the above
      (real-world frequency is much lower outside specific patterns),
      but needed for full spec coverage and some libraries' internals;
      fine to defer to the end of this phase or fold into Phase 7
      alongside async iteration if that's a cheaper joint implementation.
- [ ] Regenerate/extend the fork-built test suite pattern from the
      original implementation (a standalone driver exercising each new
      construct) for every addition here — this phase is pure "does the
      parser/interpreter accept and correctly evaluate X," which is
      exactly what a small targeted test script per feature is for.

### Phase 6 — Prototype object model + core built-ins

- [ ] A real prototype chain: every object has an internal `[[Prototype]]`
      link, property lookup walks it, `Object.create`/
      `Object.getPrototypeOf`/`Object.setPrototypeOf` work, `class`
      (Phase 5) desugars onto this rather than being a separate parallel
      object model.
- [ ] `Object.prototype` methods (`hasOwnProperty`, `toString`,
      `valueOf`), `Object.keys`/`values`/`entries`/`assign`/`freeze`/
      `defineProperty`/`defineProperties`/`getOwnPropertyDescriptor` —
      `defineProperty` with real getter/setter support matters
      specifically because some libraries (and occasionally React
      internals/polyfills) rely on it.
- [ ] Full `Array.prototype`: `map`/`filter`/`reduce`/`forEach`/`find`/
      `findIndex`/`some`/`every`/`slice`/`splice`/`concat`/`join`/
      `sort`/`reverse`/`includes`/`indexOf`/`flat`/`flatMap`/`push`/
      `pop`/`shift`/`unshift` — real arrays (`Array.isArray` true,
      proper `length` semantics), not the ad-hoc object-with-numeric-
      keys approximation from the original build.
- [ ] Full `String.prototype`: `split`/`slice`/`substring`/`replace`/
      `replaceAll`/`trim`/`padStart`/`padEnd`/`toUpperCase`/
      `toLowerCase`/`includes`/`startsWith`/`endsWith`/`repeat`/
      template-literal tag functions.
- [ ] `Math`, `Number` (parsing, formatting, `isInteger`/`isFinite`/
      `isNaN`), `JSON.parse`/`JSON.stringify` (real recursive
      serialization, not a stub).
- [ ] `RegExp` — **reuse `src/regex.cpp`**, this codebase's existing
      regex engine (already used for `:s`/`:g` and search), rather than
      writing a second one. Wrap it behind the `RegExp`
      object/`String.prototype.match`/`replace`/`test`/`exec` surface.
      This is the single biggest "don't reinvent" opportunity in this
      whole plan — check `regex.cpp`'s actual feature coverage against
      JS regex syntax (character classes, groups, backreferences,
      common flags) before assuming a 1:1 fit; extend it rather than
      forking a separate engine if there's a gap.
- [ ] `Map`/`Set`/`WeakMap`/`WeakSet`, `Symbol` (at least well-known
      symbols like `Symbol.iterator`, needed for `for...of` over custom
      iterables — real iterables in general, not just arrays/strings).
- [ ] `Function.prototype.call`/`apply`/`bind`.
- [ ] `Error` and its subtypes (`TypeError`/`RangeError`/etc.) as real
      objects with `.message`/`.stack` (a best-effort stack, not
      necessarily line-accurate), `try`/`catch`/`finally`/`throw`
      already exist at the statement level per the original build — this
      phase is about making the *thrown values* behave like real Error
      objects rather than plain strings.
- [ ] `Proxy`/`Reflect` — genuinely lower priority (real-world frequency
      outside specific state-management libraries is low), defer unless
      Part III's React milestone specifically needs it (React itself
      doesn't require Proxy for its core; some newer state libraries do
      — cross this bridge if/when it's actually blocking something).

### Phase 7 — Async: Promises, microtasks, async/await

- [ ] A real microtask queue, integrated with Part III Phase 9's event
      loop (this phase and that one are tightly coupled — build them
      together or in immediate succession, not independently).
- [ ] `Promise` (`resolve`/`reject`/`then`/`catch`/`finally`,
      `Promise.all`/`race`/`allSettled`), spec-correct resolution timing
      (a `.then` callback runs as a microtask, never synchronously, even
      if the promise is already settled) — get this exactly right, since
      subtle timing bugs here are exactly the kind of thing that makes
      "looks like it works" code silently misbehave under real load.
- [ ] `async`/`await` — implementable either as real interpreter-level
      coroutine support (suspend/resume the tree-walker's own call
      stack) or as a desugaring to Promise chains at parse time; the
      desugaring approach is very likely the pragmatic choice for a
      tree-walking interpreter (a real coroutine-capable interpreter is
      a much bigger lift) — note this as an explicit design decision
      when implemented, not an accident.
- [ ] `queueMicrotask`.
- [ ] Verify with a script that does `fetch`-shaped async control flow
      (even before Part IV's real `fetch()` exists — fake the underlying
      I/O with a `setTimeout`-based stub) to confirm ordering/timing
      matches real JS semantics (a classic test: log ordering across
      sync code, a resolved promise's `.then`, and a `setTimeout(fn, 0)`
      should interleave in the spec-defined order, not whatever's
      convenient to implement).

### Phase 8 — Memory model: real garbage collection

Today's interpreter runs one bounded script execution per `RunScripts`
call and relies on C++ RAII to clean everything up when it returns. Once
Part III Phase 9 makes scripts keep running indefinitely across frames
(closures captured by event listeners, timers, Promise chains — all
outliving any single "call" boundary), that stops being sufficient:
reference cycles (a closure capturing a DOM node that itself references
the closure via an event listener, extremely common in real JS) will leak
memory for the lifetime of the pane.

- [ ] Choose and implement a real collection strategy — mark-and-sweep
      is the pragmatic choice for a tree-walking interpreter (simpler to
      retrofit onto existing object/closure representations than
      generational or incremental GC; pause-time doesn't need to be
      as tight as a production JS engine's, since this isn't running at
      60fps-critical scale). Every JS-visible allocation (objects,
      arrays, closures, and their captured environments) needs to be
      GC-tracked; DOM nodes referenced *from* JS need a way to be rooted
      without leaking the whole DOM tree's lifetime into the GC's own
      bookkeeping (a DOM node isn't a JS-GC-owned object — it's owned by
      `HtmlDoc`'s tree — so this needs a clean ownership boundary, not
      just treating `DomNode*` as another GC root).
- [ ] Run the collector on a sensible cadence relative to Part III's
      event loop (e.g. after N script executions, or when heap size
      crosses a threshold) — not so eager it thrashes, not so lazy the
      pane's memory grows unbounded during a long interactive session.
- [ ] Stress-test with a script that deliberately creates closures
      capturing DOM references in a loop across many simulated
      event-loop ticks, confirming memory stays bounded rather than
      growing per tick.

---

## Part III — Making it a live application host

**This is the part that actually matters for "run frameworks like
React."** Parts I/II give the engine correct rendering and language
semantics; this part gives it the *capabilities* a real interactive web
app fundamentally requires — nothing built here is optional if the goal
is "React mounts and a button click updates the UI."

### Phase 9 — Event loop integration

- [ ] `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval` — a real
      timer queue, checked against wall-clock time once per mep frame
      (mirrors how `mep.on_frame`-driven Lua timers already work
      elsewhere in this codebase — same polling-once-per-frame idiom,
      just for JS timers instead of Lua ones).
- [ ] `requestAnimationFrame`/`cancelAnimationFrame` — React's own
      scheduler (and most animation code) leans on this heavily; fire
      once per mep frame while the HTML pane is the active/visible pane
      (no need to fire for a backgrounded pane, mirroring how e.g. a
      backgrounded terminal pane's own live-grid update already has a
      "is this actually being looked at" gate elsewhere in `editor.cpp`).
- [ ] **Continuous script execution**: scripts (and their registered
      callbacks/timers/listeners) need to keep running across frames for
      as long as the HTML pane exists, not just once at parse time. This
      is the core architectural shift — `HtmlSession` needs a live
      `js_engine` instance (not a one-shot `RunScripts` call) that
      persists for the pane's lifetime, and mep's own per-frame update
      (wherever `DrawPane`/`Editor::HandleInput` currently only touch
      the HTML pane for scrolling/zoom) needs to pump that engine's
      timer/microtask/animation-frame queues every frame the pane is
      open, whether or not it's currently focused (a `setInterval` in a
      backgrounded tab still fires in a real browser, within reason —
      match that unless there's a strong perf reason not to).
- [ ] Re-layout-on-mutation: once JS can keep running and keeps mutating
      the DOM (Phase 10), `DrawPane`'s "recompute layout every frame"
      approach (Part 0 baseline) is actually the *right* fit here — no
      separate "did the DOM change, do I need to relayout" tracking is
      needed as long as layout stays cheap enough (Part VI Phase 21 is
      where that assumption gets revisited once real app-sized DOM trees
      make "just relayout everything every frame" too slow).
- [ ] Decide and document the interaction between mep's own modal input
      model (Normal/Insert/etc.) and a *running* JS app's own timers/
      animation frames — do they keep firing while the user has switched
      focus to a different pane entirely, or only while `Mode::Html` is
      the active mode? Match real-browser-tab semantics (keeps running
      in the background, within a frame budget) unless there's a
      concrete reason to diverge; write down whichever choice is made.
- [ ] Verify with a `setInterval`-driven clock/counter script running
      continuously and visibly updating on-screen without any further
      user input, confirming the loop genuinely keeps ticking frame over
      frame.

### Phase 10 — Full DOM mutation API

Today: `document.getElementById` + `.textContent` get/set is the entire
mutation surface. Real apps (React's DOM renderer very much included)
need the actual DOM tree-editing API.

- [ ] `document.createElement(tag)`, `document.createTextNode(text)`.
- [ ] `node.appendChild`/`insertBefore`/`removeChild`/`replaceChild`/
      `remove()`, `node.cloneNode(deep)`.
- [ ] `element.setAttribute`/`getAttribute`/`removeAttribute`/
      `hasAttribute`.
- [ ] `element.classList` (`add`/`remove`/`toggle`/`contains`) —
      React (and virtually every real app) drives visual state changes
      through class toggling constantly; this is not optional.
- [ ] `element.style` as a real JS object with camelCase property
      accessors (`el.style.backgroundColor = 'red'`) that read/write the
      underlying inline `style=""` declarations, plus
      `window.getComputedStyle(el)` for reading the *resolved* style
      (post-cascade, not just inline) — needs Phase 2's cascade engine
      to answer correctly.
- [ ] Tree navigation properties: `parentNode`/`parentElement`/
      `children`/`childNodes`/`firstChild`/`lastChild`/`nextSibling`/
      `previousSibling`/`nextElementSibling`/`previousElementSibling`.
- [ ] `element.innerHTML` get/set (set = re-parse the assigned string as
      HTML and replace children — reuses `ParseHtml`'s tokenizer/tree-
      builder against a fragment rather than a full document) and
      `outerHTML`. Explicitly deferred in the original build; this is
      where it lands.
- [ ] `document.body`/`document.head`/`document.documentElement`.
- [ ] Every mutation here needs to trigger the "re-layout, since layout
      recomputes fresh each frame anyway" path from Phase 9 — confirm
      there's no stale-cache path left over anywhere that could show
      pre-mutation content for even one frame.
- [ ] Verify with a script that builds a small UI fragment
      (`createElement` + `appendChild` a few nested nodes, no framework
      involved yet) entirely via this API with no static HTML for it in
      the source page, confirming it renders identically to the
      equivalent static markup would.

### Phase 11 — Event system

- [ ] `addEventListener`/`removeEventListener` on any DOM node (and
      `document`/`window`), a real listener registry per node.
- [ ] Synthetic event objects: at minimum `Event`, `MouseEvent` (click,
      mousedown/up, mouseover/out — `type`, `target`, `currentTarget`,
      `clientX`/`clientY`), `KeyboardEvent` (`key`, `code`,
      `ctrlKey`/`shiftKey`/etc.) — `preventDefault()`/`stopPropagation()`/
      `stopImmediatePropagation()` need to actually do something
      (suppress whatever default mep-side behavior the event would
      otherwise trigger, and halt further dispatch, respectively).
- [ ] Real capture + bubble dispatch order (capture phase root-to-
      target, then bubble phase target-to-root) — React's own event
      system (even in versions that use a single root listener
      internally) depends on bubbling semantics being correct, since
      it's built *on top of* the real DOM event model, not a
      replacement for it.
- [ ] **Wire mep's own input handling into this**: when the HTML pane is
      focused (`Mode::Html`) and the mouse clicks / a key is pressed
      within the pane's content rect, hit-test against the current
      layout (`HtmlLayout`'s runs/boxes from Part I — this needs each
      laid-out box to know which `DomNode` it came from, which the
      current `HtmlRun`/`HtmlRule` structs in `main.cpp` don't track
      yet, so add that back-reference here) to find the target element,
      then dispatch a synthetic event into the JS engine the same way a
      real browser's own input layer feeds its DOM event pipeline. This
      is the other half of Phase 9's "live application host" shift —
      Phase 9 makes JS keep running, this phase makes it *react to the
      user*.
- [ ] `element.focus()`/`.blur()`, a tracked "currently focused element"
      concept (needed for real `:focus` CSS from Phase 2, and for form
      input keyboard routing from Phase 3's widgets).
- [ ] `CustomEvent`, `element.dispatchEvent` (needed for a page's own
      scripts to fire synthetic events at each other, a common pattern
      in real component libraries).
- [ ] Verify with a page containing a plain (no-framework) button whose
      `onclick`/`addEventListener('click', ...)` handler mutates the DOM
      (Phase 10) in a way that's visibly different on screen — confirm a
      real xdotool-driven click on the rendered button in Xvfb actually
      triggers it, not just a simulated/faked event call from a test
      script. This is the last checkpoint before attempting Phase 12.

### Phase 12 — Milestone: run a real React app

Not a phase with new engine capability of its own — this is the
integration checkpoint that validates everything Parts I–III actually
built, against the real thing rather than hand-written test pages.

- [ ] Take an unmodified, real `react` + `react-dom` UMD (or a small
      `create-react-app`/Vite-built bundle) — something with genuine
      `useState`-driven interactivity, e.g. a counter with an increment
      button — and load it via `:Browse` against a local HTML file
      referencing the bundle.
- [ ] It should: parse without errors, execute React's own bootstrap
      code, mount the initial UI (visible, correctly laid out per
      whatever CSS the app ships), and — this is the actual bar — a real
      xdotool click on the rendered increment button should visibly
      update the counter on screen, exactly the way it would in a real
      browser.
- [ ] Whatever breaks first (and something will) becomes the next
      concrete, prioritized work item — real error messages from a real
      unmodified bundle are a far better prioritization signal at this
      point than more speculative phase-planning. Expect this to
      surface gaps not explicitly called out above (an obscure `Array`
      method, a DOM property nobody thought to list, a timing edge case
      in the microtask queue) — that's the point of the milestone.
- [ ] Once a basic counter works, a natural stretch goal (not required
      to consider Part III "done," but a good next checkpoint) is a
      slightly larger real app: something with a list render (`.map`
      over data → JSX, i.e. `React.createElement` calls), conditional
      rendering, and a text input bound to state — exercises Phase 3's
      form widgets + Phase 11's keyboard events together.

---

## Part IV — Networking & resource loading

### Phase 13 — Subresource fetching

- [ ] Parse and resolve `<link rel="stylesheet" href="...">`,
      `<script src="...">`, and `<img src="...">` (*local* `<img>` paths
      already render for real, not a placeholder -- see the Addendum
      above; only `http(s)://` sources still fall back to the bracketed
      placeholder) against the page's own base URL (the page's own URL,
      or `<base href>` if present).
- [ ] Each fetch is a `mep.job_start`-spawned `curl` call, same
      mechanism as the top-level page fetch — parallelize reasonably
      (don't serialize N image fetches one at a time) but keep it
      simple; a real browser's own connection-pooling/prioritization
      logic is out of scope.
- [ ] Fetched stylesheets feed into Phase 2's `CollectStyleRules`
      pipeline exactly like an inline `<style>` block does, just sourced
      from a fetched string instead of the DOM tree's own text content.
- [ ] Fetched images decode via the same path Phase 4 (or, if this
      phase lands after Phase 4, reuse — check ordering) established for
      local images; consider reusing `image_doc.h`'s existing decoder
      (`ImageDoc`) directly rather than a second image-decoding path,
      the same "don't reinvent" instinct as Phase 6's `regex.cpp` reuse.
- [ ] Fetched external scripts (`<script src>`) get appended to
      `HtmlDoc::scripts` (or an equivalent ordered list respecting
      `defer`/`async` attribute semantics if cheap — synchronous-in-
      document-order is an acceptable v1 simplification, same as the
      Part 0 baseline's existing inline-script-only model) and run
      through the same `RunScripts` pipeline once fetched.
- [ ] Verify with a real (simple) external page that has a separate CSS
      file and a couple of `<img>` tags, confirming all of it loads and
      renders.

### Phase 14 — `fetch()`/`XMLHttpRequest` from JS

- [ ] `fetch(url, options)` returning a real `Promise` (Phase 7) that
      resolves to a `Response`-shaped object (`.status`, `.ok`,
      `.json()`, `.text()`, `.headers`) — backed by the same curl-
      subprocess primitive as Phase 13, wired through Phase 9's event
      loop (the curl job's completion callback resolves the Promise on
      a future tick, not synchronously).
- [ ] `XMLHttpRequest` (the older, callback/event-based API) — many
      real-world scripts and some libraries still use this directly;
      implement as a thin wrapper over the same underlying fetch
      primitive rather than a fully separate code path.
- [ ] **Safety boundary**: page JS's network access should go through
      this one code path, spawning curl the same sandboxed way every
      other subprocess in this codebase does — no raw socket API, no
      filesystem access, no ability to reach `mep.job_start` with an
      arbitrary command (the JS engine has never had, and must not
      gain, a generic "run a shell command" binding). Worth an explicit
      short design note here on exactly what a page's fetch() *can* and
      *can't* reach, since this is the first phase where page JS gets
      any real-world I/O capability at all.
- [ ] Verify with a script that `fetch()`s a small local test JSON file
      (served via a trivial local file:// or a `python -m http.server`
      instance spun up just for the test) and renders the result into
      the DOM.

### Phase 15 — Cookies, storage, history

- [ ] `localStorage`/`sessionStorage` — a simple in-memory key-value
      store per origin is sufficient for `sessionStorage`;
      `localStorage` should persist to disk (a small JSON file under
      mep's own data dir, mirroring `MepDataDir()`'s existing convention
      for `projects.json`) so it survives across mep restarts, matching
      real browser semantics.
- [ ] `document.cookie` get/set — a basic cookie jar; real expiry/
      domain/path scoping rules are worth getting approximately right
      (enough that a typical login-flow or preference cookie behaves
      sensibly) without chasing every edge case of the spec.
- [ ] `history.pushState`/`replaceState`/`popstate` event — needed for
      client-side-routed SPAs (React Router and equivalents) to update
      the visible "URL" without a full page reload; mep's own pane
      header (`HtmlSession::source`) is the natural place to reflect the
      current pushState-updated URL.
- [ ] `window.location` (read-mostly: `.href`/`.pathname`/`.search`/
      `.hash`; a `.href` *write* triggering real navigation is a bigger
      scope question — does it re-fetch and replace the whole
      HtmlSession the way a fresh `:Browse` would? Decide and document
      when implemented, rather than leaving it an inconsistent partial
      behavior).
- [ ] Verify with a tiny two-view client-side router (pushState-driven,
      no framework needed) confirming back/forward-equivalent behavior
      makes sense within a single pane (there's no browser back button
      here — decide what, if anything, mep-side maps to `popstate`;
      even "nothing does yet, it's just spec-correct plumbing for a
      future keybinding" is a fine answer to land with, noted as such).

---

## Part V — Visual fidelity

### Phase 16 — Flexbox

Near-required for real-world CSS — the large majority of React apps and
modern hand-written sites use `display: flex` for basic layout, not just
elaborate ones.

- [ ] `display: flex`, `flex-direction` (row/column, plus the `-reverse`
      variants), `justify-content`, `align-items`, `align-content`,
      `flex-wrap`.
- [ ] Per-item `flex-grow`/`flex-shrink`/`flex-basis` (or the `flex`
      shorthand), `align-self`, `order`.
- [ ] `gap`/`row-gap`/`column-gap`.
- [ ] This slots into `HtmlLayoutBlock` (`main.cpp`) as a new layout
      mode alongside the existing block/inline flow, selected per
      container by its own `display` value from Phase 1's box model —
      not a replacement for block/inline layout, an addition.
- [ ] Verify with a real-world-shaped flex layout (a header bar: logo
      left, nav links centered/right, `justify-content: space-between`)
      and a wrapping card grid (`flex-wrap: wrap` + `gap`).

### Phase 17 — CSS Grid

Increasingly common in real modern CSS; a real second layout mode
alongside flexbox.

- [ ] `display: grid`, `grid-template-columns`/`grid-template-rows`
      (fixed sizes, `fr` units, and `repeat()` at minimum),
      `grid-template-areas` if the fixed-track version is solid,
      `gap`, `justify-items`/`align-items`/`justify-content`/
      `align-content`.
- [ ] Per-item `grid-column`/`grid-row` placement (`span N` at minimum;
      named lines are a further stretch).
- [ ] Lower priority than Phase 16 — ship flexbox fully working first,
      since it covers far more real-world content per unit of effort.

### Phase 18 — Transforms, transitions, basic animations

- [ ] `transform: translate/scale/rotate` (2D only — 3D transforms are
      Part VII-class scope), applied at draw time via the same `rlgl`
      matrix-push technique the italic-shear rendering already
      demonstrates is viable in this codebase.
- [ ] `transition` on a small useful property set (opacity, transform,
      background-color, color) — needs a per-element "animating from X
      to Y over duration D" tracked state, advanced once per frame via
      Phase 9's event loop's own per-frame pump, with an easing-function
      table (linear + the standard `ease`/`ease-in`/`ease-in-out` cubic-
      bezier approximations).
- [ ] `@keyframes`/`animation` — same underlying per-frame-advance
      mechanism as `transition`, generalized to multiple keyframe stops.
- [ ] `opacity` as a real compositing property (blend against whatever's
      already drawn underneath, or at minimum against the pane's flat
      background) — currently there's no alpha-blending concept in the
      renderer at all outside raylib's own per-draw-call `Color.a`.
- [ ] Verify with a hover-triggered `transition` (needs Phase 2's
      `:hover` to actually be live, i.e. after Part III's event/focus
      tracking exists) and a looping CSS `animation` (a simple pulsing
      or sliding element), confirmed smooth via a short recorded GIF
      (`gif_creator`) rather than a single static screenshot, since
      motion is the whole point being verified here.

### Phase 19 — Pseudo-elements, remaining pseudo-classes, effects

- [ ] `::before`/`::after` with `content:` — extremely common in real
      CSS for icons/decorative content; implemented as synthetic
      (non-DOM, JS-invisible) child nodes generated at style-compute
      time from the matched rule's `content` value.
- [ ] `:hover`/`:focus`/`:active` going fully live (Phase 2 stubbed the
      matching; this is where the *state* driving them — current mouse
      position, focused element — becomes real, once Part III's event
      system exists to track it).
- [ ] `border-radius`, `box-shadow` (a soft drop-shadow approximation is
      fine — real Gaussian blur is expensive and rarely worth pixel-
      perfect fidelity here), `outline`.
- [ ] Real `overflow: hidden`/`scroll`/`auto` as actual per-element
      clipped, independently-scrollable sub-regions (today only the
      whole pane scrolls, via `HtmlSession::scroll_y`) — this needs a
      per-element scroll-offset concept and nested `BeginScissorMode`
      regions in the renderer, plus its own mouse-wheel/drag hit-testing
      once Part III's event system can route input to the right
      sub-region instead of always the pane-level scroll.
- [ ] `z-index` / real stacking contexts (currently everything paints in
      DOM/document order with no stacking-context concept) —
      `position: absolute/fixed` from Phase 1 and `z-index` here are
      closely related and may be worth doing together if Phase 1 ends
      up deferring real positioning to here; check Phase 1's own scope
      when this phase starts and adjust the split if needed.
- [ ] Verify with a card component using `::before` for a decorative
      icon, rounded corners + shadow, and an absolutely-positioned
      badge with a higher `z-index` than its sibling content.

---

## Part VI — Performance

### Phase 20 — Bytecode compiler + VM

A tree-walking interpreter re-evaluates the AST on every execution of
every statement/expression — fine for a handful of lines running once,
genuinely too slow for React's own runtime (tens of thousands of function
calls during a single render pass) executing continuously across frames
once Part III's event loop is real.

- [ ] Design a bytecode instruction set covering everything Part II's
      language surface needs (arithmetic/comparison/logical ops, local/
      global/closure variable access, property get/set, function calls,
      control flow as jumps, the object/array/prototype model from
      Phase 6).
- [ ] A compiler pass from the existing AST (Part II's parser output,
      unchanged) to bytecode — the parser/AST layer stays exactly as-is;
      only the *evaluation* strategy changes, so this is additive, not a
      rewrite of Phases 5–8's language-surface work.
- [ ] A register- or stack-based VM executing the bytecode — stack-based
      is the simpler, more conventional choice for a project at this
      scale (matches how most non-JIT bytecode interpreters, e.g.
      Python's or Lua's own, are built) and is the recommended default
      here.
- [ ] This is explicitly **not** a JIT — compiling to native machine
      code is an enormous additional undertaking (register allocation,
      platform-specific codegen, deoptimization) that real JS engine
      teams spend years on; a well-implemented bytecode VM alone
      typically gets an order of magnitude or more over a naive tree-
      walker, which is very likely sufficient for this project's actual
      needs (an interactive app in a single editor pane, not a
      benchmark-competitive general-purpose JS runtime).
- [ ] Sequence this **after** Parts II/III's correctness work is
      substantially done, not before — optimizing an interpreter whose
      semantics are still actively changing (new operators, async,
      classes) means re-doing bytecode-generation work repeatedly.
      Revisit this ordering only if Phase 12's React milestone is
      concretely blocked on raw execution speed rather than missing
      features.
- [ ] Verify via a before/after timing comparison on a real workload
      (Phase 12's React counter app, or a synthetic tight-loop
      benchmark) — this phase's success criterion is a measured speedup,
      not just "it still passes the same tests."

### Phase 21 — Incremental layout & paint

- [ ] Dirty-tracking: a DOM mutation (Phase 10) marks the affected
      subtree (and anything whose layout depends on it — a changed
      child height can affect an ancestor's own size) as needing
      relayout, instead of Phase 9's "just relayout everything every
      frame" baseline.
- [ ] Layout caching per node (position/size from the last valid layout
      pass), invalidated only along the dirty path — real apps with
      large DOM trees (hundreds+ of nodes) need this once "relayout
      everything every frame" measurably shows up as dropped frames.
- [ ] Paint-level culling refinement: the Part 0 baseline already has a
      cheap per-run vertical-visibility skip in `DrawPane`'s draw loop;
      revisit whether that's still sufficient once real page sizes/DOM
      complexity from Part III's React milestone are the normal case, or
      whether a real spatial index (even a simple sorted-by-y list with
      binary search for the visible range) is warranted.
- [ ] Verify via the same before/after timing methodology as Phase 20,
      against a real app with meaningful interaction-driven re-renders
      (not just initial page load, where a naive full relayout is
      already cheap enough not to matter).

---

## Part VII — The long tail (open-ended, genuine "full WebKit parity")

Deliberately left at survey depth — see **Scale check** above for why.
Each item below is a real, substantial engineering effort in its own
right (multiple of these are bigger than this entire plan's Parts I–VI
combined, at a real browser vendor's scale); write a proper phase-by-
phase breakdown for whichever one comes up first as an actual blocker for
a real page/app someone wants to view, rather than speculatively
designing all of them now.

- [ ] **Canvas 2D API** (`<canvas>` + `CanvasRenderingContext2D`) — paths,
      fills/strokes, gradients, image data manipulation, text metrics.
      High real-world value (widely used for charts/graphics/games) and
      probably the single most approachable Part VII item, since it maps
      fairly directly onto raylib's own existing 2D drawing primitives.
- [ ] **SVG** — a second, declarative vector-graphics document format
      with its own element set, largely orthogonal to the HTML/CSS
      engine built in Parts I–VI; real-world pages embed SVG constantly
      (icons especially), making this higher-value than it might first
      appear.
- [ ] **WebGL / WebGPU** — full 3D graphics APIs; a genuinely enormous
      undertaking (essentially "implement OpenGL ES's/a modern GPU
      API's JS binding surface, correctly, on top of raylib's own GL
      context"). Lowest priority of this list for a text-editor's
      browser pane.
- [ ] **`<audio>`/`<video>` + codec decoding** — real audio/video codec
      support (needs vendoring a codec library — ffmpeg or similar —
      plus a media pipeline) is a large, separate scope from anything
      else in this plan.
- [ ] **Web Components / Shadow DOM** — custom elements, encapsulated
      style/DOM scoping; increasingly used by some real sites/design
      systems, orthogonal extension to Part III's DOM model.
- [ ] **Service Workers, WebSockets, WebAssembly execution** — three
      independent, each-substantial pieces: an offline/caching proxy
      layer, a persistent bidirectional socket API (needs Phase 14's
      networking layer extended with a long-lived connection type, not
      just request/response), and (most ambitiously) actually executing
      `.wasm` binaries — real sites increasingly ship WASM, so this one
      in particular may become worth prioritizing sooner than its
      position in this list suggests.
- [ ] **Accessibility tree / ARIA** — exposing a real accessibility tree
      (for screen readers etc.) alongside the visual render tree; low
      priority given mep's own primary interface is already a keyboard-
      driven terminal-adjacent editor rather than a general-audience
      browser, but worth a real look if this pane ever becomes a primary
      way users consume content rather than a developer-tool preview.
- [ ] **Internationalization**: bidirectional text (RTL scripts),
      complex script shaping (Arabic/Indic/CJK), IME input support for
      form fields (Phase 3's widgets) — real, substantial typography/
      text-shaping engineering, well beyond a "few checkboxes" scope.
- [ ] **Security hardening / process isolation**: real browsers run each
      site (sometimes each tab) in its own sandboxed OS process
      specifically so a compromised renderer can't touch the rest of the
      system. mep's in-house engine runs entirely in-process with the
      rest of the editor — revisit this once Part IV's networking
      surface (fetch/XHR reaching real, untrusted remote content) is
      wide enough that the current "no filesystem/subprocess API
      exposed to JS" boundary (Design decisions, above) stops feeling
      like sufficient isolation on its own. A real fix here (separate
      process, IPC boundary) is a significant architecture change, not a
      phase-sized task — flag it honestly as a standing risk rather than
      pretending a checklist item closes it.

---

## Explicitly out of scope (noted so it isn't re-litigated)

- **Replacing the in-house engine with an embedded real one** (V8/JSC/
  QuickJS/Hermes for JS; a vendored layout engine for CSS) — the
  explicit premise of this whole feature, per the user's own request
  when it was first built, is that it's built in this repo. If a future
  session seriously reconsiders this trade-off, that's a decision for
  the user to make explicitly, not something to drift into
  incrementally by "just this one dependency."
- **Pixel-identical rendering versus real Chrome/Safari/Firefox** — not
  a goal at any point in this plan, including after Part VII. "Correct
  enough that real content is usable and a real framework runs" is the
  bar throughout, not "indistinguishable in a diff tool."
- **Removing the external WebKit escape hatch** (`mep.browse()`/
  `:BrowseExternal`) — stays available indefinitely, including after
  this entire plan is notionally "done," as the fallback for whatever
  the in-house engine still can't handle.
- **A general-purpose, standalone browser product** — this stays a mep
  *pane*, integrated with mep's own pane/buffer/mode model throughout;
  no plan here to spin it out into a separate application, add tab
  management independent of mep's own tabs, a bookmarks bar, browser
  history UI beyond Phase 15's spec-level `history` API, etc.
