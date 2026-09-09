# stb_truetype Removal Plan

Replace `third_party/stb_truetype.h` (vendored single-header,
`STB_TRUETYPE_IMPLEMENTATION` compiled into
`src/gfx/backend_native_text.cpp`) with an in-house TrueType/OpenType
glyph-outline parser and scanline rasterizer, behind the same
`gfx::LoadFontData`/`GenImageFontAtlas`/`LoadFontFromMemory`/
`UnloadFontData` API `gfx/text.h` already defines -- zero call-site
changes anywhere outside `backend_native_text.cpp` itself.

**How to resume:** check the boxes below, `git log --oneline -- src/gfx/backend_native_text.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> verify (screenshot-compare glyph rendering against the
current stb_truetype-backed output, saved beforehand) -> tick the box ->
next phase, pausing for the user's go-ahead unless told to keep going.

---

## Current usage (grounded in `backend_native_text.cpp`, 361 lines)

Only **7 stb_truetype functions** are called anywhere in the codebase:

- `stbtt_InitFont` -- parse a font file's tables, build the `stbtt_fontinfo`
  handle every other call takes.
- `stbtt_ScaleForPixelHeight` -- convert a target pixel size into the
  font-units-to-pixels scale factor.
- `stbtt_GetFontVMetrics` -- ascent/descent/line-gap (this session's own
  alignment-bug fix added this call: `LoadFontData` wasn't adding ascent
  to `offsetY`, unlike raylib's own implementation -- see git history for
  the fix and its live-verified before/after screenshots).
- `stbtt_GetCodepointHMetrics` -- per-glyph advance width + left side
  bearing.
- `stbtt_GetCodepointBitmap` -- rasterize one glyph to a coverage bitmap
  at a given scale (this is the actual outline-parse + scanline-fill
  work; everything else here is table lookups).
- `stbtt_FreeBitmap` -- release that bitmap.

No kerning-table (`stbtt_GetCodepointKernAdvance`), no vector/SDF output,
no variable-font axes, no hinting instruction execution (`stbtt`'s own
rasterizer doesn't execute TrueType hint bytecode either -- it does pure
scanline coverage rasterization from the raw outline, which is exactly
what's being proposed here too, so this is genuine parity, not a scoped-
down subset). This is a narrow, well-understood surface: mep needs
*outline parsing + coverage rasterization*, not a general-purpose font
shaping engine (no ligatures/complex-script shaping either -- consistent
with `gfx/text.h`'s own flat per-codepoint API).

## Scoping decisions

1. **`glyf`-table TrueType outlines only** (quadratic Bezier contours),
   not CFF/PostScript (`CFF `-table, cubic Bezier + Type2 charstring
   interpreter) outlines. Check every font mep actually ships/loads
   (`third_party_licenses/JetBrainsMono-OFL.txt`,
   `LiberationSans-OFL.txt`, `NerdFonts-OFL.txt`,
   `NotoSansSymbols2-OFL.txt` -- the font files themselves are wherever
   `main.cpp`'s font-loading code points, check there in Phase 1) before
   assuming this is sufficient; JetBrains Mono and most Nerd Font/Noto
   builds ship `glyf` outlines, but confirm rather than assume. CFF
   support (needed only if a shipped font turns out to be CFF-flavored
   OpenType) is a documented fallback, not blocking. **Update after
   Phase 1: one font (`kSymbolFontTtf`) *did* turn out to be CFF-flavored
   -- resolved by re-subsetting it from Noto Sans Symbols 2's TrueType
   release instead of adding a CFF interpreter, see Phase 1's own notes.
   `glyf`-only stayed correct as a scoping decision for the actual
   rasterizer; the font asset itself was the thing that needed to
   change.**
2. **No hinting bytecode execution** -- matches stb_truetype's own
   behavior (see above), so this is a like-for-like swap, not a quality
   regression.
3. **Scanline coverage rasterization**, same algorithmic family
   stb_truetype uses (active-edge-list scanline fill with anti-aliasing
   via fractional coverage), not a from-scratch novel rasterizer design
   -- this is a well-documented, implementable algorithm, not a research
   problem.

## Phases

### Phase 1: font inventory + format confirmation
- [x] Found every embedded font (`font_data.h`, `icon_font_data.h`,
  `symbol_font_data.h`, `office_font_data.h`/`_serif.h`/`_mono.h` -- 15
  fonts total across JetBrains Mono, the icon font, the symbol fallback
  font, and 12 Liberation Sans/Serif/Mono weights) and checked each
  one's sfnt table directory directly (extracted the byte arrays from
  the headers with a small script, no original `.ttf` files exist
  in-tree). **Result: 14 of 15 are `glyf`-flavored TrueType as expected
  -- but `kSymbolFontTtf` (the Noto Sans Symbols 2 fallback font) is
  `OTTO`/`CFF `-flavored.** Its own header comment explained why: the
  original subset was built from `NotoSansSymbols2-Regular.otf`
  (Google Fonts' CFF release), not the `.ttf` one, since it didn't
  matter with stb_truetype in the picture.
- [x] Confirmed this font is genuinely load-bearing (used in
  `TerminalCellFont`/general text glyph lookup for any codepoint in
  `kSymbolCodepointRanges`, not a rare corner case) before deciding how
  to handle it.
- [x] **Resolved by re-subsetting from Google Fonts' TrueType release**
  instead of adding a CFF/Type2 charstring interpreter: downloaded
  `github.com/google/fonts/raw/main/ofl/notosanssymbols2/NotoSansSymbols2-Regular.ttf`
  (confirmed genuine `glyf` flavor), regenerated the exact same subset
  via `pyftsubset` (via `nix shell nixpkgs#python3Packages.fonttools`,
  same `kSymbolCodepointRanges` unicode list), verified full coverage
  (281/281 codepoints, matching the original exactly, via a `ttx` cmap
  dump), and wrote the new byte array into `symbol_font_data.h` with an
  updated top comment documenting why and the new regenerate recipe.
  Same license, same upstream family, ~10KB larger (glyf outlines are
  less compact than CFF charstrings) -- a good trade against not needing
  a second outline format's worth of parser/interpreter code for one
  font.
- [x] Also confirmed (same script) which fonts need cmap format 12
  (full-Unicode, not just BMP): **JetBrains Mono and the icon font
  both do** (supplementary-plane Private-Use-Area icon codepoints above
  U+FFFF -- `kIconCodepointExtras` in `main.cpp` has several), so format
  12 support was added to Phase 2's scope alongside format 4, not left
  as a "maybe."

### Phase 2: table parsing
- [x] `sfnt` container: table directory parse, `head`/`hhea`/`maxp`/
  `hmtx`/`cmap`/`loca`/`glyf` table lookup. `cmap` subtable selection
  prefers format 12 (full Unicode) over format 4 (BMP-only), matching
  Phase 1's finding that JetBrains Mono and the icon font both need it.
  All in `src/gfx/truetype.cpp`'s `InitFont`.
- [x] `GetFontVMetrics`/`GetCodepointHMetrics`: direct `hhea`/`hmtx`
  reads, including the hmtx "trailing glyphs repeat the last advance,
  own lsb-only array" compaction rule for `glyph_index >=
  numberOfHMetrics`.

### Phase 3: outline parsing
- [x] `glyf` simple-glyph parsing: flag/coordinate delta decoding per
  the spec.
- [x] The "expand implied on-curve midpoints between consecutive
  off-curve points, then rotate to start on an on-curve point" TrueType
  contour convention -- confirmed this was indeed the fiddliest spec
  detail, implemented as its own `BuildEdgesForContour` step so the
  actual line/quadratic walk afterward never has to special-case
  consecutive off-curve points.
- [x] Composite glyphs: component records (word/byte args, xy-offset or
  point-matching -- the latter skipped gracefully, unused by any shipped
  font), scale/2x2 transforms (`WE_HAVE_A_SCALE`/`_AN_X_AND_Y_SCALE`/
  `_A_TWO_BY_TWO`), recursive resolution with a depth guard. Confirmed
  necessary, not hypothetical: the Liberation families' accented Latin
  glyphs use them.

### Phase 4: scanline rasterization
- [x] Quadratic Beziers flattened via recursive de Casteljau subdivision
  with a flatness stopping criterion (chord-deviation tolerance), not a
  fixed segment count -- adapts segment density to each curve's actual
  size instead of over- or under-tessellating.
- [x] Rasterization approach ended up as a **hybrid**, not pure
  active-edge-list: horizontal coverage computed exactly from real edge
  x-intersections (fractional pixel coverage, nonzero winding rule), and
  antialiasing done via a fixed 4x vertical supersample per pixel row.
  Chosen over full 2D analytic coverage during implementation for
  correctness-simplicity (fewer edge cases to get right) -- glyph atlas
  baking is a one-time startup cost, not per-frame, so the extra
  sub-scanline passes cost nothing that matters. `GetCodepointBitmap`
  matches `stbtt_GetCodepointBitmap`'s exact xoff/yoff sign convention
  (font y-up -> raster y-down, yoff negative for a glyph above the
  baseline) so `LoadFontData`'s ascent-offset math needed zero changes.

### Phase 5: swap-in + verification
- [x] New standalone module `src/gfx/truetype.h`/`.cpp` (not folded
  into `backend_native_text.cpp` -- substantial enough, and reusable/
  testable on its own, to warrant its own file) in the `gfx::tt`
  namespace; `backend_native_text.cpp`'s 6 call sites (`stbtt_InitFont`/
  `_ScaleForPixelHeight`/`_GetFontVMetrics`/`_GetCodepointHMetrics`/
  `_GetCodepointBitmap`/`_FreeBitmap`) retargeted 1:1, `#include
  "../third_party/stb_truetype.h"` and its `STB_TRUETYPE_IMPLEMENTATION`
  block removed. Added `src/gfx/truetype.cpp` to `MEP_GFX_NATIVE_SOURCES`
  in `CMakeLists.txt`.
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  clean build, zero warnings under mep's own `-Werror` strict flags on
  the first real build (no iteration needed). `nix develop --command
  just test` -- pure-logic suite passes (same pre-existing/unrelated
  `mep-collab-session-test` gap as every other plan in this series).
- [x] `mep-gfx-native-integration-smoke` against the real embedded
  JetBrains Mono TTF: clean, correctly antialiased "mep gfx native
  backend" text alongside the 2D/3D primitives it already drew.
- [x] Live verification in a real `mep` instance: opened `:term`, the
  starship prompt rendered correctly end to end -- JetBrains Mono body
  text, AND the icon font's Nerd Font glyphs (git-branch icon, version
  icons, the Deno/Nix icons, the arrow prompt), which specifically
  exercises cmap format 12 lookup, composite-glyph resolution, and the
  rasterizer together, not just simple-glyph ASCII text.
- [x] Standalone probe of the regenerated symbol font specifically
  (`gfx::LoadFontFromMemory` with an explicit codepoint list spanning
  `kSymbolCodepointRanges`, including U+23F5 and U+2733 -- the exact two
  codepoints `symbol_font_data.h`'s own comment cites as the original
  motivating real-world bug): all requested in-range codepoints
  rendered as correct, clean glyph shapes; the handful of out-of-range
  codepoints in the same test correctly fell back to the missing-glyph
  placeholder, confirming that's genuine `kSymbolCodepointRanges`
  coverage, not a hole introduced by the CFF->glyf font swap.
- [x] Deleted `third_party/stb_truetype.h` and
  `third_party_licenses/stb_truetype-LICENSE.txt` (`git rm`) once
  `grep -rln "stbtt_\|stb_truetype\|STB_TRUETYPE"` across `src/`/
  `CMakeLists.txt`/`flake.nix` returned only comment-level mentions
  (this plan's own prose, `gfx/truetype.h`/`.cpp`'s own historical
  references, and two genuinely-historical unrelated mentions in
  `pdf_doc.h`/`office_font_data.h` describing a since-abandoned old PDF
  renderer prototype -- left alone, not live code). Rebuilt clean with
  the vendored header fully absent to confirm no stray include survived.

## Non-goals

- CFF/PostScript outline support -- only add if Phase 1 finds a shipped
  font actually needs it.
- Kerning, ligatures, complex-script shaping, variable-font axes, color
  fonts (`COLR`/`CBDT`) -- stb_truetype doesn't provide these either, so
  this isn't a regression; out of scope for parity.
- Hinting bytecode execution -- explicitly not needed (see Scoping
  decision 2).
