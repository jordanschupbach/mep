# WYSIWYG Office-Pane Toolbar — Implementation Plan

Goal (user ask, verbatim intent): give the Office pane (`.docx`/`.odt`,
`Mode::OfficeNormal/Insert/Visual`) a more standard set of WYSIWYG editor
features — font family/size pickers, Bold/Italic/Underline/Strikethrough
buttons, alignment buttons, special-character insertion, tables, images,
math-mode text, and any other common editor features. Implement all of it,
checking items off below as they land, so work can resume cleanly if this
session gets interrupted.

Scope boundaries (deliberate, matching this codebase's existing "v1, not a
Word/LibreOffice clone" philosophy from NVIM_PARITY_PLAN.md Phase 44):
**out of scope** — headers/footers, footnotes/comments, track changes,
real multi-level numbered lists, page breaks/sections, hyperlink
*authoring* (reading/preserving existing links already works), spell
check, image resize-drag-handles, merged table cells, nested tables,
per-cell rich formatting inside tables, system-clipboard integration
beyond whatever already exists. Math is rendered via mep's existing
LaTeX-subset math engine (the same one org-mode/HTML `<math>` uses), not
real OOXML/ODF math (OMML/MathML) — round-trips as plain formatted text
within mep, a documented v1 limitation.

Every new feature must degrade gracefully on load/save (skip cleanly,
never crash/corrupt), matching the existing tolerance convention.

## Progress checklist

### 0. Research / groundwork
- [x] Read office_doc.h/.cpp (span-editing primitives, DOCX read+save) in full.
- [x] Generate + embed Liberation Serif + Liberation Mono (4 variants each)
      as `src/office_font_data_serif.h` / `src/office_font_data_mono.h`,
      matching `office_font_data.h`'s existing format (fonts sourced from
      the same OFL-licensed Liberation family already on-disk locally).
- [x] Confirm native (C++) prompt mechanism: none exists (`BeginPrompt`
      only takes a Lua ref) — adding a `prompt_native_callback_`
      `std::function<void(const std::string&)>` path is the plan.
- [x] Math API confirmed: `LayoutMathExpression(latex, font_size)` ->
      `MathLayoutResult`; `DrawMathLayout(x, y, result, color)`. Always
      succeeds (bad input just degrades to plain text).
- [x] Image loading confirmed: `ImageDoc::LoadFromMemory(bytes, len)`
      already works on arbitrary in-memory bytes; existing texture cache
      assumes a real file path for mtime staleness -- office embedded
      images need their own cache keyed by a synthetic id instead.
- [x] office_odt.cpp table/image skip points confirmed: DOCX's
      `LoadDocxFromMemory` only iterates `body.children("w:p")` (`<w:tbl>`
      never visited); ODT's paragraph walker branches by tag name with no
      `table:table` case. Both are simple "add a branch" insertion points.
- [x] Keybinding audit done: OfficeNormal/Insert/Visual's full current key
      list confirmed (see editor.cpp ~2104-2416); `s` is free for
      strikethrough in Visual mode next to b/i/u.

### 1. Data model (office_doc.h/.cpp)
- [x] `DocFormat`: font_family/font_size_pt/superscript/subscript/
      has_color+rgb/has_highlight+rgb/math fields added, `operator==`/`!=`
      updated.
- [x] `DocParagraph`: `bullet` bool replaced with `ListKind {None, Bullet,
      Numbered}`; numbered's displayed number is computed at render time
      (position within the current run), not stored.
- [x] `DocTable`/`DocImage` structs + `OfficeDoc::tables`/`images` +
      per-paragraph `table_ref`/`image_ref` anchors declared (not yet
      wired into load/save/render/editing -- section 7/8 below).
- [x] `SetFormatFieldOverRange` (span-range, non-boolean fields) and
      `SetParagraphAlignment`/`SetParagraphListKind` (whole-paragraph
      range) implemented in office_doc.cpp.
- [x] DOCX read+write extended: `<w:rFonts>`/`<w:sz>`/`<w:color>`/
      `<w:shd>`/`<w:vertAlign>` <-> font_family/font_size_pt/color/
      highlight/superscript/subscript. Font-family recognized by name
      heuristic (exact Liberation name or common metric-compatible
      substitutes -- Arial->Sans, Times/Georgia/Cambria->Serif,
      Courier/Consolas->Mono).
- [x] ODT read+write extended the same way (`style:font-name`/
      `fo:font-size`/`fo:color`/`fo:background-color`/
      `style:text-position`); style-cache key widened from a packed int
      bitfield to a string (needed once fields aren't all booleans).
- [x] List-kind round-trip: DOCX `<w:numPr>` presence still reads as
      Bullet (distinguishing Numbered would need resolving numbering.xml's
      abstractNum format, out of v1 scope); **list membership is still
      dropped on save for both formats** (pre-existing limitation, now
      documented to cover Numbered too, not just Bullet) -- authoring a
      list in mep works and round-trips within mep's own session
      (undo/redo, live editing), just not through a save+reopen cycle yet.
- [x] Build verified clean after this section.

### 2. Font embedding + rendering (main.cpp)
- [x] Load the 8 new Font atlases (Serif/Mono × 4 weight-style) at startup
      alongside the existing 4 Sans ones, same baked-once convention.
- [x] `OfficeFontFor(DocFormat)` picks among all 3 families.
- [x] Superscript/subscript: baseline Y-offset + 0.65x size scale; explicit
      font_size_pt override -- both applied in the main per-run draw loop
      only (cursor-position/selection-highlight measurement loops still use
      the outer uniform paragraph size, a documented cosmetic-only
      approximation for mixed-size lines, never a correctness/data issue).
- [x] Text color / highlight color: per-run draw color override + a
      background rect behind the run, both in the main draw loop.
- [x] Math run rendering wired into the same loop (LayoutMathExpression/
      DrawMathLayout in place of DrawTextEx for a DocFormat.math run).
- [x] Build verified clean.

### 2b. Editor:: setters + native prompt (editor.h/.cpp)
- [x] `Editor::BeginPromptNative` (std::function callback path alongside
      the existing Lua-ref `BeginPrompt`), `HandlePromptInput` updated to
      check it first.
- [x] `ApplyOfficeFormatFieldOverSelection` (shared Visual-selection-vs-
      cursor dispatch) + SetOfficeFontFamily/FontSizePt/Color/Highlight
      (+Clear variants)/ToggleOfficeSuperscript/Subscript.
- [x] `SetOfficeAlignment`/`OfficeAlignmentActive`,
      `SetOfficeListKind`/`OfficeListKindActive` (paragraph-range, toggles
      back to None if already uniformly set).
- [x] `InsertOfficeText` (special chars), `InsertOfficeMath` (native
      prompt -> math-flagged span), `InsertOfficeTablePrompt` (two chained
      native prompts: rows then cols), `InsertOfficeImagePrompt` (native
      prompt for a path, decodes via ImageDoc to confirm + capture dims).
- [x] `EnterOfficeTable`/`ExitOfficeTable`/`MoveOfficeTableCell` + new
      `OfficeSession` fields (`in_table_edit`, `table_cursor_row/col`,
      `table_cell_editing`).
- [x] `SetOfficeZoom` (simple clamped multiplier -- the richer "settle-band
      folding into base_font_pt" behavior OfficeSession::zoom's own old
      comment described was never actually wired to a keybinding before
      this, so this is genuinely new, not a restoration).
- [x] `ToggleOfficeFormat`/`OfficeFormatActive` extended for 's'
      (strikethrough).
- [x] Keybindings: HandleOfficeVisualInput's b/i/u branch simplified to
      call the shared `ToggleOfficeFormat` (also now handles 's'); added
      c/L/r/f (align center/left/right/justify), */# (bullet/numbered),
      ^/_ (super/subscript).
- [x] Build verified clean.

### 3. Toolbar UI (main.cpp DrawPane office branch)
- [x] Restructure into a wider/multi-row toolbar: font family dropdown,
      font size dropdown, B/I/U/S buttons, align×4 buttons, bullet/numbered
      buttons, superscript/subscript buttons, text-color + highlight-color
      swatch buttons (small preset palette popup), special-char button,
      math button, table-insert button, image-insert button, undo/redo
      buttons, zoom −/+ buttons.
- [x] Lightweight native dropdown/popup widget (open below a button, click
      an item; no click-away/Escape-close yet, closes only by re-clicking
      the toggle button or picking an item — used by font family, font
      size, color swatches, and special-char grid). Math entry uses the
      native prompt (`BeginPromptNative`), not this popup widget.
- [x] `Editor::` setters for each new toolbar action, mirroring
      `ToggleOfficeFormat`/`OfficeFormatActive`'s existing pattern.
- [x] Fixed two draw/click-order bugs found live under Xvfb: (1) the
      generic "click anywhere in the pane body focuses this pane"
      catch-all region was registered before the toolbar's own button
      regions and swallowed every toolbar click (fixed by shrinking that
      region's height to exclude the toolbar rows); (2) dropdown popups
      were drawn *before* the paragraph content loop, so the document
      painted over them afterward (looked like giant garbled text) and,
      separately, popup item clicks landed inside the same catch-all focus
      region and were swallowed the same way. Fixed by moving popup
      drawing to after the content loop's `EndScissorMode()` (so it paints
      on top, unclipped) and skipping the catch-all focus-click region
      entirely whenever a dropdown is open. Verified via Xvfb screenshots:
      Font, Size, Tc (color), and Sym (special chars) popups all open,
      render at the correct size in the right place, and route clicks to
      the correct item.

### 4. Keybindings (HandleOfficeVisualInput, mirroring toolbar actions)
- [x] Strikethrough (`s`), alignment (`c`/`L`/`r`/`f`), bullet/numbered
      toggle (`*`/`#`), superscript/subscript (`^`/`_`) — landed alongside
      the b/i/u simplification in section 2b.

### 5. Math mode
- [x] Insert/edit an inline math span (LaTeX subset) via toolbar button
      (`fx`) + native prompt; renders via `LayoutMathExpression`/
      `DrawMathLayout` in place of literal glyphs for `DocFormat.math` runs.

### 6. Special characters
- [x] Popup grid of common symbols (©®™€£¥§¶°±×÷≈≠≤≥…–—½¼¾→←↑↓αβγδπΩ),
      click inserts at cursor. Verified opening/rendering/click-insert live
      under Xvfb.

### 7. Tables
- [x] Insert-table toolbar button (`Tbl`, chained native prompts for
      rows/cols), default cell text empty. Auto-enters the new table
      (cursor at row0/col0) right after insert.
- [x] Rendering: grid lines + cell text in the paragraph flow at the
      anchor point, current cell highlighted, cursor drawn while editing.
- [x] Navigation/editing: hjkl/arrows and Tab/Shift-Tab between cells
      (wrapping row-to-row), `i`/`a` to edit a cell's plain text (Escape or
      Enter ends cell edit). Stepping off the top/bottom row exits the
      table and resumes normal paragraph motion; landing on a table-anchor
      paragraph via j/k/gg/G auto-enters it. Two real bugs found and fixed
      live under Xvfb: (1) the toolbar's own catch-all pane-focus click
      region was swallowing every click *inside* the table too whenever a
      dropdown was open, same root cause as the toolbar-popup click bug in
      section 3; (2) Escape-ing out of a table anchored at the very
      first/last paragraph re-entered it immediately on the next j/k press
      since goto_para's auto-enter fired even on a clamped-to-the-same-
      paragraph no-op move -- fixed by only auto-entering on an actual
      paragraph change.
- [x] DOCX round-trip: parses every `<w:tbl>` in body order into a
      `DocTable` + fresh empty anchor paragraph; serializes back with
      explicit single-line borders on every edge + equal-width columns
      (DocTable has no per-column width model). Verified: `unzip -t` clean,
      `soffice --headless --convert-to pdf` renders a correctly bordered
      table with all cell text intact.
- [x] ODT round-trip: parses every `<table:table>` the same way; serializes
      back with one shared bordered `table-cell` style registered in
      automatic-styles. Same soffice-PDF verification, same result.

### 8. Images
- [x] Insert-image toolbar button (`Img`, native prompt for a local file
      path), decoded via the existing `ImageDoc` in-memory loader
      (already implemented in section 2b).
- [x] Rendering: draws the texture inline in the paragraph flow at the
      anchor point, capped to the pane's content width, via a new
      buffer_id+image_ref-keyed texture cache (`GetOrLoadOfficeImageTexture`,
      mirroring `GetOrLoadImageTexture`'s own per-buffer cache).
- [x] DOCX round-trip: load resolves `<w:drawing>`'s `<a:blip r:embed>` via
      `word/_rels/document.xml.rels` and reads the target `word/media/`
      part; save writes each `DocImage` as a fresh `word/media/mepimageN.*`
      part (extension sniffed from magic bytes) + a `rIdMepN` relationship
      + a self-contained `wp:inline`/`a:graphic`/`pic:pic` drawing (all
      needed namespaces declared inline, not depending on the root
      element), adding missing `[Content_Types].xml` Default entries as
      needed, all via `WriteZipReplacingEntries`. A pre-existing image is
      always re-added as a new part on save rather than reusing its
      original one (documented v1 simplification -- correct, if not
      maximally space-efficient across repeated saves).
- [x] ODT round-trip: load resolves `<draw:image xlink:href>` (already a
      package-root-relative path) directly; save writes each image to a
      fresh `Pictures/mepimageN.*` part + a `META-INF/manifest.xml`
      `file-entry`, wrapped in a `draw:frame` (`text:anchor-type="as-char"`
      so it flows inline). Both round-trips verified end-to-end: insert
      live under Xvfb, save, `unzip -t` clean, media part + rel/manifest
      entry present, `soffice --headless --convert-to pdf` renders the
      image at the correct position.

### 9. Misc common features
- [x] Undo/redo toolbar buttons (`Un`/`Re`, wired to existing
      `UndoOffice`/`RedoOffice` in section 2b/3).
- [x] Zoom −/+ toolbar buttons (`Z-`/`Z+`, existing `OfficeSession::zoom`
      field, `SetOfficeZoom` from section 2b).

### 10. Verification
- [x] Build clean after each major chunk (data model, toolbar, tables,
      images) -- every chunk in this plan was followed by a full
      `cmake --build` before moving on.
- [x] Live Xvfb pass: opened a real `.docx`, exercised the toolbar's font/
      size/color/special-char dropdowns, inserted+navigated+edited a
      table, inserted an image, saved, confirmed `unzip -t` reports no
      errors and the file reopens correctly in mep with the new content
      intact. Repeated for `.odt`. Cross-checked both with
      `soffice --headless --convert-to pdf`, matching Phase 44's own
      independent-verification convention -- all four (docx table, docx
      image, odt table, odt image) render correctly with real content and
      visible borders in the LibreOffice-produced PDF.

---
*Update this checklist as each item lands — check the box, add a one-line
implementation note if the approach differs from what's written above.*
