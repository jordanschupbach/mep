# stb_image / stb_image_write Removal Plan

Replace `third_party/stb_image.h` and `third_party/stb_image_write.h`
(vendored single-headers, `STB_IMAGE_IMPLEMENTATION`/
`STB_IMAGE_WRITE_IMPLEMENTATION` compiled into `src/image_doc.cpp` and
`src/gfx/backend_native_renderer2d.cpp` respectively) with in-house image
codecs, behind the same call shapes (`stbi_load`/`stbi_load_from_memory`/
`stbi_write_png`/`stbi_write_png_to_func`) their four call sites already
use.

**How to resume:** check the boxes below, `git log --oneline -- src/image_doc.cpp src/gfx/backend_native_renderer2d.cpp src/gfx/backend_native_model_obj.cpp src/gfx/backend_native_model_gltf.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> verify (screenshot-compare + decode real sample images)
-> tick the box -> next phase, pausing for the user's go-ahead unless
told to keep going.

**Depends on [MINIZ_REMOVAL_PLAN.md](MINIZ_REMOVAL_PLAN.md) landing
first** -- PNG decode/encode needs a DEFLATE codec, and that plan
already scopes writing one (shared with ZIP and the M3D importer). Doing
miniz first turns this plan's PNG phase into "wire up the already-built
codec," not "write DEFLATE a second time." Not a hard blocker if done
out of order, but wasteful.

---

## Current usage (grounded in reading every call site)

Only **4 call sites**, no calls anywhere in `main.cpp` itself (everything
goes through the `gfx::` bridge already, same pattern raylib's own
removal established):

- `image_doc.cpp:50` -- `stbi_load_from_memory(bytes, len, &w, &h, &channels, 4)`,
  mep's general-purpose image-buffer decoder (the image editor's own
  document model). Compiled with `STBI_ONLY_PNG`/`STBI_ONLY_JPEG`/
  `STBI_ONLY_BMP`/`STBI_ONLY_GIF` -- **this #define list is the actual
  format scope to match**, not stb_image's full format support (which
  also includes PSD/HDR/PIC/PNM -- none of those flags are set, so
  mep never asked for them).
- `backend_native_renderer2d.cpp:484` -- `stbi_load(file_name, ...)`,
  the `gfx::LoadImage`-from-path backend (same format scope).
- `backend_native_renderer2d.cpp:545,559` -- `stbi_write_png`/
  `stbi_write_png_to_func`, backing `gfx::ExportImage` and the
  screenshot/render-to-image agent RPCs (`ui.screenshot`,
  `model.renderToImage`) -- **PNG encode only**, no other write format
  is ever called.
- `backend_native_model_obj.cpp:91` / `backend_native_model_gltf.cpp:366`
  -- `stbi_load`/`stbi_load_from_memory` for OBJ/glTF material textures
  -- same format scope, no new requirement.

So the real scope is: **decode PNG + JPEG + BMP + GIF, encode PNG only.**
No HDR/PSD/animated-GIF-frame-sequence handling (only the first GIF
frame is ever needed -- confirm during Phase 1 whether any current call
site actually feeds an animated GIF, since `stbi_load`'s plain (non
`_gif`) entry point already only returns the first frame, matching
what's used today).

## Scoping decisions

1. **PNG decode/encode shares `MINIZ_REMOVAL_PLAN.md`'s DEFLATE codec**
   (zlib-wrapped variant -- PNG's `IDAT` chunks are RFC 1950 zlib
   streams, filter bytes per scanline per PNG's own spec on top). This
   plan's PNG phase is "PNG container format (chunks, filters, color
   types) + call the shared codec," not "write DEFLATE again."
2. **JPEG: baseline DCT only**, not progressive JPEG, not arithmetic
   coding (a rarely-used, patent-encumbered variant even stb_image
   doesn't support), matching stb_image's own default scope reasonably
   closely. Baseline JPEG (Huffman-coded, sequential DCT) covers the
   overwhelming majority of real-world `.jpg` files (it's what every
   camera/phone/web export tool produces by default); progressive JPEG
   (rarer, used for some "high quality" web exports) is Phase 5's
   documented follow-up, not blocking.
3. **BMP: uncompressed only** (`BI_RGB`), 24/32-bit -- covers what any
   modern tool actually writes; RLE-compressed BMP (`BI_RLE8`/`BI_RLE4`)
   is a legacy format essentially never produced today.
4. **GIF: first frame only**, matching current usage (no call site
   requests animation frames). LZW decode (GIF's own compression, a
   *different* algorithm from DEFLATE -- simpler, no Huffman coding, just
   the LZW dictionary scheme) is still required.

## Phases

### Phase 1: format-usage audit
- [x] Confirmed: zero `.jpg`/`.jpeg`/`.gif`/`.bmp` files anywhere in the
  repo (only 2 real PNG fixtures exist -- `test/r_plot.png` and
  `tmp_assets/rocket_v2.png`), so nothing in-tree exercises Scoping
  decisions 2-4's excluded variants (progressive JPEG, RLE BMP, animated
  GIF) -- they stay genuinely "unused," not just "probably unused."
  **Verified via:** direct chunk-level inspection (not just `file`) of
  both PNG fixtures: `r_plot.png` is 480x480, color type 3 (indexed,
  8-bit, no tRNS -- so a real-world palette image, but no PLTE+tRNS
  combination to worry about for these two), split across 4 `IDAT`
  chunks (confirms the "concatenate before decompressing" requirement is
  real, not theoretical); `rocket_v2.png` is 400x400, color type 6
  (truecolor+alpha, 8-bit), single `IDAT`. Both non-interlaced. tRNS
  support is still included in Phase 2 below despite neither fixture
  using it -- common enough in real-world palette PNGs generally to be
  worth the low incremental cost, not scope creep.

### Phase 2: PNG (decode + encode)
- [x] `src/png_codec.h`/`.cpp`. Chunk parser: `IHDR`, `PLTE`, `tRNS`
  (both forms -- per-palette-entry alpha for indexed color, and the
  single-transparent-sample-value form for grayscale/truecolor,
  included despite neither real fixture using it -- see Phase 1),
  `IDAT` (concatenated across chunks), `IEND`; interlaced (Adam7) images
  rejected with an error rather than silently mis-decoded. All 5 color
  types (grayscale/truecolor/indexed/grayscale+alpha/truecolor+alpha)
  at bit depths 1/2/4/8/16, always decoded to RGBA8 output (matching
  every real call site's fixed `desired_channels=4`, confirmed in
  Phase 1 -- no bit-depth/channel-count preservation needed).
- [x] Decompression via `deflate::InflateZlib` (`MINIZ_REMOVAL_PLAN.md`).
- [x] Per-scanline filter reversal (all 5 types: None/Sub/Up/Average/
  Paeth).
- [x] Encode path: per-scanline "minimum sum of absolute differences"
  filter selection (tries all 5 filter types per row, keeps the
  smallest -- the same heuristic real encoders including libpng's
  default use, meaningfully better than always "None" for what the
  DEFLATE stage compresses afterward), `deflate::DeflateZlib`
  compression, `IHDR`/`IDAT`/`IEND` chunks with correct CRC-32
  (`deflate::Crc32` -- PNG uses the identical algorithm ZIP does).
  Supports `comp` 1/2/3/4 (gray/gray+alpha/RGB/RGBA), matching
  `stbi_write_png`'s own parameter -- though every real call site only
  ever passes 4.
- [x] `png::Encode` returns a `std::string` (the complete file bytes) --
  `backend_native_renderer2d.cpp`'s `stbi_write_png_to_func` call site
  (in-memory PNG export) and its `stbi_write_png` (direct-to-file) call
  site both wrap this one function differently rather than needing two
  separate encode paths (see Phase 5).
- [x] **Verified via:** both real in-tree PNG fixtures (`test/r_plot.png`
  -- 480x480 indexed/8-bit, `tmp_assets/rocket_v2.png` -- 400x400
  truecolor+alpha/8-bit) decoded byte-identical to ImageMagick's own
  independent `-depth 8 rgba:` extraction. 5 additional hand-constructed
  synthetic PNGs (valid files built directly via Python's `struct`/
  `zlib`, not through this codec) covering every remaining case real
  fixtures didn't exercise -- grayscale 8-bit, grayscale 1-bit (packed/
  sub-byte sample unpacking), RGB 16-bit (high-byte truncation),
  grayscale+alpha 8-bit, and grayscale with single-color tRNS -- all
  matched ImageMagick's independent decode exactly. Encoder verified two
  ways: round-tripped through this same module's own decoder (exact
  match, both fixtures) and, separately, its output independently
  re-decoded by ImageMagick (exact match) -- confirming real-world PNG
  compliance, not just internal self-consistency.

### Phase 3: JPEG decode (baseline only)
- [x] Marker-segment parser (`SOI`/`APPn`/`DQT`/`SOF0`/`DHT`/`DRI`/`SOS`/`EOI`),
  rejecting non-baseline SOF variants (progressive/arithmetic/lossless).
- [x] Huffman decode of entropy-coded MCU (minimum coded unit) data: a
  JPEG-specific canonical Huffman table builder (distinct from
  `deflate.cpp`'s -- JPEG transmits its own (BITS,VALUES) representation
  directly, spec Annex C), DC signed-magnitude diff decoding + AC
  run/size decoding with ZRL/EOB handling, byte-stuffing-aware bit
  reader, restart-marker (RST0-RST7) resync resetting DC predictors and
  byte-aligning the bitstream.
- [x] Dequantization, inverse DCT (direct separable row/column
  floating-point IDCT -- correctness prioritized over AAN-style speed
  tricks per this plan's own note), chroma upsampling (nearest/box
  replication, matching stb_image's own non-"fancy" default -- *not*
  libjpeg's default smooth/triangle-filter upsampling, see verification
  note below), YCbCr -> RGB color conversion (grayscale images pass
  through the single component directly as R=G=B).
- [x] `src/jpeg_codec.h`/`.cpp`. Compiled clean standalone against the
  full `MEP_STRICT_FLAGS` set (`nix develop --command g++ ... -Werror`).
- [x] **Verified via:** real JPEGs generated from the two in-tree PNG
  fixtures (`tmp_assets/rocket_v2.png`, `test/r_plot.png`) at multiple
  chroma-subsampling factors -- `cjpeg`/`convert` producing 4:2:0, 4:4:4,
  4:2:2, a grayscale-source JPEG, and a restart-marker JPEG
  (`cjpeg -restart 4`, confirmed 6 RST markers present in the file) --
  decoded and compared pixel-by-pixel against `djpeg -nosmooth` (an
  independent decoder, libjpeg-turbo, with its own *fancy* chroma
  upsampling explicitly disabled to match this decoder's intentional
  simple-upsampling scope decision -- confirmed as the actual source of
  an early ~50-70/255 max-diff false alarm against `djpeg`'s smooth-
  upsampling default before `-nosmooth` was used). All 6 files matched
  within max diff 1-3 per channel (DCT rounding tolerance), 0-3 samples
  out of 480000-691200 exceeding a tolerance of 2. Additionally verified
  the raw luma plane alone (before any chroma upsampling/color
  conversion) byte-for-byte against `djpeg -grayscale` (which decodes
  pure Y with no chroma influence) with 0 mismatches beyond ±1 rounding
  -- isolating the DCT/dequantization/Huffman-decode path as fully
  correct independent of the upsampling-choice difference. Error
  handling verified against a truncated file, a non-JPEG file, and an
  empty file -- all fail gracefully with a descriptive `*out_error`, no
  crash.
- **Bug found and fixed during verification:** quantization tables in
  the `DQT` marker are transmitted in zigzag scan order (spec Annex
  B.2.4.1), not natural row-major order -- the initial implementation
  stored them as read (zigzag order) but the dequantization step
  indexed them by natural-order position (matching how decoded
  coefficients are stored after zigzag placement), causing every
  dequantized coefficient to be multiplied by the wrong quantization
  step. Fixed by dezigzagging the table on load (`quant_tables[table_id][kZigzag[i]] = ...`),
  confirmed by the verification above dropping from ~40-230 max diff to
  1-3.

### Phase 4: BMP + GIF decode
- [x] `src/bmp_codec.h`/`.cpp`. Header parse (`BITMAPFILEHEADER` +
  `BITMAPINFOHEADER`/V2/V3/V4/V5 variants, detected via the DIB header's
  own size field), direct pixel copy for `BI_RGB` 16/24/32-bit plus
  `BI_BITFIELDS` (generic R/G/B/A bitmask extraction with bit-width
  rescaling to 8 bits) -- widened slightly past the original "BI_RGB
  only" scoping note after finding real-world encoders (ImageMagick's
  own default 32-bit-with-alpha export) actually write
  `BI_BITFIELDS`/`BITMAPV5HEADER`, which the plan's own "what modern
  tools actually write" criterion argues for supporting. Row order
  (bottom-up unless height is negative) verified against both a
  synthetic top-down and bottom-up 16-bit fixture.
- [x] `src/gif_codec.h`/`.cpp`. Logical screen descriptor, global/local
  color tables, LZW decompression (a hand-written `LzwDecoder` --
  GIF's own variable-code-width dictionary scheme starting at
  `min_code_size + 1` bits and growing to a 12-bit cap, distinct from
  both DEFLATE's LZ77+Huffman and TIFF's fixed-growth-point LZW),
  Graphic Control Extension (transparency index) parsing, 4-pass
  interlace de-interlacing, first-frame-only image descriptor + pixel
  data composited over a background-filled logical-screen-sized canvas.
- [x] Both compiled clean standalone against the full `MEP_STRICT_FLAGS`
  set (`nix develop --command g++ ... -Werror`).
- [x] **Verified via:** BMP -- real fixtures generated from
  `tmp_assets/rocket_v2.png` at 24-bit `BI_RGB` and 32-bit
  `BI_BITFIELDS`/`BITMAPV5HEADER` (ImageMagick's own default), both
  matched ImageMagick's independent `-depth 8 rgba:` decode
  byte-for-byte after fixing a found bug (below); a hand-constructed
  synthetic 16-bit 555 BMP in both bottom-up and top-down row order,
  matched a hand-computed expected-pixels file exactly. GIF -- real
  fixtures generated from both in-tree PNGs (opaque, transparent via
  `-background none`, and interlaced via `-interlace GIF`), all matched
  ImageMagick's independent decode byte-for-byte (0 mismatches across
  RGB and the binary alpha channel, all 4 fixtures). Error handling for
  both verified against a truncated file, a non-{BMP,GIF} file, and an
  empty file -- all fail gracefully with a descriptive `*out_error`, no
  crash.
- **Bug found and fixed during verification:** the BMP 24/32-bit
  `BI_RGB` default color masks had R and B swapped -- BMP stores pixel
  bytes as B,G,R (low-to-high in memory), but the initial
  implementation packed those bytes into an integer and then assigned
  the *low* byte position to the R mask instead of B, silently
  swapping red and blue on every chromatic (non-gray) pixel. Caught by
  comparing against ImageMagick's decode (11520/160000 R/B channel
  samples mismatched, all at chromatic pixels -- gray pixels have R==B
  so the swap was invisible there, which is why the bug wasn't
  obvious from a visual scan); fixed by swapping the mask assignment
  (`r_mask = 0xFF0000`, `b_mask = 0x0000FF`), confirmed by the
  now-exact match above.

### Phase 5: swap-in + verification
- [x] New `src/image_codec.h`/`.cpp`: a magic-byte format-sniffing
  dispatcher (PNG `\x89PNG`, JPEG `\xFF\xD8`, BMP `BM`, GIF `GIF8[79]a`)
  over the four format-specific decoders, plus a `DecodeFile` helper
  (read-then-`Decode`) for the two call sites that previously used
  `stbi_load(path, ...)` directly. Compiled clean standalone against the
  full `MEP_STRICT_FLAGS` set.
- [x] Retargeted all 5 call sites:
  - `image_doc.cpp` -- `stbi_load_from_memory`/`stbi_image_free` ->
    `image_codec::Decode`/`std::free`; dropped the whole
    `STB_IMAGE_STATIC`/`STBI_ONLY_*`/warning-suppression preamble that
    existed only to vendor stb_image.h safely alongside raylib's own
    copy (raylib is gone; nothing to dodge anymore).
  - `backend_native_renderer2d.cpp` -- `stbi_load` -> `image_codec::DecodeFile`
    (`LoadImage`); `stbi_write_png`/`stbi_write_png_to_func` ->
    `png::Encode` + a plain `fopen`/`fwrite` or `std::vector` copy
    (`ExportImage`/`ExportImageToMemory`); dropped the
    `STB_IMAGE_IMPLEMENTATION`/`STB_IMAGE_WRITE_IMPLEMENTATION` block
    and its own comment about the eventual single-definition conflict
    with `image_doc.cpp` once this file joins `mep_core` -- moot now,
    both TUs just call into the same shared codec library.
  - `backend_native_model_obj.cpp` -- `stbi_load`/`stbi_image_free` ->
    `image_codec::DecodeFile`/`std::free` (OBJ `map_Kd` texture import).
  - `backend_native_model_gltf.cpp` -- `stbi_load_from_memory`/
    `stbi_image_free` -> `image_codec::Decode`/`std::free` (glTF
    embedded/external image import).
  - All 5 sites' `unsigned char*` RGBA8-out, caller-frees-via-`std::free`
    contract preserved exactly (image_codec's decoders `std::malloc`
    their output, matching stb_image's own allocator, so no call site
    needed its free-side changed beyond the function name).
- [x] `CMakeLists.txt`: added `png_codec.cpp`/`jpeg_codec.cpp`/
  `bmp_codec.cpp`/`gif_codec.cpp`/`image_codec.cpp` to `mep_core`'s
  source list and to the two standalone targets that build
  `MEP_GFX_NATIVE_SOURCES` directly without linking `mep_core`
  (`mep_add_gfx_native_smoke`'s targets, `mep-model3d-doc-test`) --
  same pattern established by `wav_doc.cpp`/`deflate.cpp` in
  MINIAUDIO_REMOVAL_PLAN.md/MINIZ_REMOVAL_PLAN.md.
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  full clean build succeeds (`mep`, `mep_core`, all smoke-test targets,
  `mep-model3d-doc-test`), zero warnings under `MEP_STRICT_FLAGS`.
- [x] `nix develop --command just test` -- all 5 non-GUI test binaries
  pass (`mep-html-doc-test`, `mep-org-doc-test`, `mep-workspace-test`,
  `mep-model3d-doc-test`, `mep-collab-crdt-test`); `mep-collab-session-test`
  requires a `ws://` server argument the justfile recipe doesn't supply
  -- a pre-existing justfile issue unrelated to this plan (confirmed by
  running it standalone; nothing in this plan touches collab/websocket
  code), not a regression introduced here.
- [x] Decode-correctness check: see Phases 2-4's own per-format
  "Verified via" notes -- both real in-tree PNG fixtures plus JPEG/BMP/
  GIF files generated from them at multiple subsampling/bit-depth/
  color-table variations, all compared against independent decoders
  (ImageMagick, libjpeg-turbo's `djpeg`) rather than stb_image directly,
  since stb_image was already fully removed from the tree by the time
  Phase 5 swapped the call sites in (Phases 2-4 built and validated each
  decoder as a standalone module first, stb_image still present but
  unused during that window).
- [x] Live verification via the agent-RPC-driven running `mep` instance
  (rebuilt from the final, stb_image-free tree): opened `rocket_420.jpg`
  (JPEG decode), `rocket_trans.gif` (GIF decode + transparency),
  `rocket32.bmp` (BMP/BITFIELDS decode), and `rplot.jpg` (grayscale JPEG
  decode) in the image editor via `file.open` -- all rendered correctly
  in `ui.screenshot` captures (which is itself the PNG-encode path,
  `ExportImage`, exercised on every capture taken this whole session).
  Also opened `greenman.glb` (a real glTF sample with an embedded PNG
  texture, confirmed via a `\x89PNG` byte search) in the 3D modeler --
  loaded successfully (356 tris, no crash/error), exercising
  `LoadGltfImageTexture`'s new `image_codec::Decode` call on the
  embedded-bufferView path.
- [x] Removed every `#include "../third_party/stb_image.h"`/
  `stb_image_write.h` and their `STB_IMAGE_IMPLEMENTATION`/
  `STB_IMAGE_STATIC`/`STBI_ONLY_*`/`STB_IMAGE_WRITE_IMPLEMENTATION`
  defines and warning-suppression pragma blocks from `image_doc.cpp`,
  `backend_native_renderer2d.cpp`, `backend_native_model_obj.cpp`,
  `backend_native_model_gltf.cpp`; deleted `third_party/stb_image.h`,
  `third_party/stb_image_write.h`, `third_party_licenses/stb_image-LICENSE.txt`,
  `third_party_licenses/stb_image_write-LICENSE.txt`. Fixed stale
  comments referencing stb_image by name in `image_doc.h`,
  `model3d_doc.h` (also corrected to describe `gfx::LoadImage`'s
  current native backend rather than a stale "via raylib's LoadImage"),
  `model3d_doc_test.cpp`, `IMAGE_EDITOR.md`, and `main.cpp` (a vestigial
  dead pragma-suppression block whose comment named
  `third_party/stb_image.h`; left the dead block itself alone as
  out-of-scope pre-existing cruft, fixed only the now-inaccurate name).

## Non-goals

- HDR, PSD, PIC, PNM decode -- never requested (`STBI_ONLY_*` flags
  confirm this), not part of this plan's scope.
- Animated GIF frame sequences -- not used by any current call site
  (Scoping decision 4).
- Progressive JPEG, arithmetic-coded JPEG, JPEG 2000 -- baseline DCT
  only (Scoping decision 2); revisit only if Phase 1's audit finds a
  real progressive-JPEG file mep needs to open.
- RLE-compressed BMP -- unused in practice (Scoping decision 3).
- Any write format beyond PNG -- no call site ever requests JPEG/BMP/GIF
  encode.
