# In-pane image editor

A raster image editor rendered entirely inside an ordinary mep `Pane`
(`Mode::ImageEditor`), the same architectural slot as the office/sheet/PDF
viewers: its own key-dispatch handler and its own `DrawPane` branch,
reachable from a plain image-viewer pane (`Mode::Image`) by pressing `e`.

Status legend: `[x]` implemented this pass, `[ ]` planned/not yet done.

## Phase 1 — core editing loop (this pass)

- [x] `ImageEditorSession`/`ImageEditorLayer` data model: ordered list of
      RGBA8 pixel layers (name, visibility, opacity, blend-normal), each
      the full canvas size.
- [x] Enter/exit: `e` on a focused `Mode::Image` pane opens the editor
      (seeded with one layer from the already-decoded `ImageDoc`); `Esc`
      or the menubar's "Close editor" returns to the plain viewer without
      discarding anything (layers/undo history stay cached, keyed by
      buffer id, so re-pressing `e` resumes exactly where you left off);
      `:w`/`:wq` flattens visible layers and writes a PNG.
- [x] Canvas: pan (drag with the Pan tool / middle mouse / Space+drag),
      zoom (`+`/`-`/`=`-to-fit, Ctrl+scroll), checkerboard behind
      transparency, nearest-neighbor scaling so pixels stay crisp.
- [x] Tools: Pencil, Eraser, Line, Rectangle (outline+fill), Ellipse
      (outline+fill), Bucket Fill (flood fill, 4-connected), Eyedropper
      (pick color from canvas), Pan.
- [x] Brush size (1-32px, `[`/`]` or a toolbar stepper).
- [x] Primary/secondary color swatches + an HSV picker popup + a
      recently-used palette strip.
- [x] Layers sidebar: add, delete, duplicate, reorder (up/down), rename,
      toggle visibility, opacity slider, click to select active layer.
- [x] Menubar: File (Save, Save As, Close), Edit (Undo, Redo), Layer (New,
      Delete, Duplicate), View (Zoom in/out/fit, Grid toggle).
- [x] Undo/redo: full-canvas snapshot stack per session (mirrors
      `OfficeSession`/`SheetSession`'s own snapshot convention), one push
      per completed stroke/shape/fill/layer-op, capped at `kMaxUndo`.
- [x] Status bar: cursor pixel position, canvas size, zoom %, active
      tool/layer.

### Keys (Mode::ImageEditor)

| Key | Action |
| --- | --- |
| `e` (while viewing a plain image, `Mode::Image`) | Open the image editor |
| `Esc` | Close the editor, back to the plain viewer |
| `b` / `x` / `l` / `r` / `c` / `f` / `i` / `h` | Pencil / Eraser / Line / Rectangle / Ellipse / Bucket fill / Eyedropper / Pan |
| `[` / `]` | Shrink / grow brush size |
| `u` / Ctrl-R | Undo / redo |
| `+` / `-` / `=` | Zoom in / out / fit to pane |
| Shift held while releasing a Line/Rectangle/Ellipse drag | Fill instead of outline |
| Middle-mouse drag (any tool) | Pan the canvas |
| Click an already-selected layer's name | Rename it |

## Phase 2 — selection & transform (not yet implemented)

- [ ] Rectangular/elliptical marquee selection; move/cut/copy/paste within
      a selection.
- [ ] Free transform (move/scale/rotate a layer or selection).
- [ ] Crop tool (resizes the whole canvas).
- [ ] Canvas resize / resample (nearest, bilinear).
- [ ] Flip/rotate layer or canvas (90°/180°/arbitrary).

## Phase 3 — richer painting (not yet implemented)

- [ ] Soft/anti-aliased brush edges; brush hardness/opacity/spacing.
- [ ] Gradient tool (linear/radial).
- [ ] Polygon/freehand lasso and freehand pen tool.
- [ ] Text tool (rasterized, using `g_font`).
- [ ] Blend modes beyond Normal (Multiply, Screen, Overlay...).
- [ ] Layer masks; clipping masks; layer groups/folders.
- [ ] Adjustment operations: brightness/contrast, hue/saturation,
      grayscale, invert, levels/curves.
- [ ] Filters: Gaussian blur, sharpen, pixelate, noise.
- [ ] Onion-skinning / reference-layer opacity for tracing.

## Phase 4 — workflow (not yet implemented)

- [ ] Multi-level undo grouping smarter than "one snapshot per stroke"
      (memory cost of full-canvas snapshots at high resolution).
- [ ] Export at a different size/format (JPEG/BMP/WebP) than the source.
- [ ] Copy layer/selection to/from the system clipboard as an image.
- [ ] wasm build support for `:w` (currently native-only — the wasm
      build's file-write bridge, `mep_js_write_file`, is text/JSON only;
      no binary-write endpoint exists yet, see `editor.cpp`'s
      `mep_js_write_file`). Until then, saving from the wasm build reports
      an explicit "not supported" error rather than corrupting the file.
- [ ] Keyboard-only tool switching cheatsheet / which-key integration.
- [ ] Configurable default canvas size / new-blank-image command.

## Non-goals (for now)

- Vector layers / SVG editing — this is a raster-only editor.
- Color management (ICC profiles) — always sRGB, matching `ImageDoc`'s
  own decode (stb_image, no profile handling).
