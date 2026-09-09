# Dependency Removal Plans

Index of every third-party dependency mep links or vendors, and whether
there's a real, worthwhile plan to replace it with an in-house
implementation -- the same raylib -> `gfx::` bridge-and-replace exercise
already completed (raylib is gone as of the `mep_core`/`main.cpp` flip to
`gfx/backend_native.cpp`; `gfx/backend_raylib.cpp`/`.h` deleted,
`FetchContent_Declare(raylib)` and `buildInputs` removed from
`CMakeLists.txt`/`flake.nix`), applied one dependency at a time instead of
all at once.

**How to resume:** pick a "Candidate" row below, open its plan file, check
the boxes as phases land (same convention as `WORKSPACES_PLAN.md`/
`WEBKIT_PARITY_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> verify -> tick the box with a short "Verified via..."
note -> next phase). Each plan file is self-contained and doesn't assume
you've read this index first, beyond the shared verification approach
below.

---

## Scope

"Dependency" here means third-party **code** mep links or vendors:
`FetchContent`-fetched libraries, `find_package`d libraries that wrap a
specific third-party codebase, and single-header libraries vendored under
`third_party/`. It deliberately excludes:

- **OS/platform APIs** -- OpenGL, X11 (core protocol + the Xtst extension
  `agent_ui_input.cpp` uses for synthetic input), `Threads::Threads`
  (pthread). These aren't swappable third-party *implementations*; they're
  the interfaces mep's own code (and GLFW, until `GLFW_REMOVAL_PLAN.md`
  lands) talks to. There's no "in-house OpenGL" to write short of a
  software rasterizer, which is out of scope entirely.
- **Build/dev tooling** -- CMake, Emscripten, the Nix devShell's compilers
  and language runtimes (`deno`, `python3`, etc. used by `just` recipes).
  Not shipped in the built binary.

## Inventory

| Dependency | Used for | Verdict | Plan |
|---|---|---|---|
| ~~raylib~~ | ~~windowing/2D/3D/text/audio/input~~ | **Done** | replaced by `gfx::backend_native` (see git history, `flake.nix`/`CMakeLists.txt` diffs) |
| Lua 5.4 | embedded scripting/config language (`lua_env.cpp`, `init.lua`) | Keep | [LUA_REMOVAL_PLAN.md](LUA_REMOVAL_PLAN.md) |
| ~~pugixml~~ | DOCX/ODT/XLSX/ODS XML parsing (`office_doc.cpp`, `office_odt.cpp`, `doc_export.cpp`, `sheet_xlsx.cpp`, `sheet_ods.cpp`) | **Done** | [PUGIXML_REMOVAL_PLAN.md](PUGIXML_REMOVAL_PLAN.md) -- replaced by `src/xml_doc.h`/`.cpp` |
| ~~miniz~~ | DOCX/ODT/XLSX/ODS ZIP container read+write, M3D's compressed body (`office_doc.cpp`, `doc_export.cpp`, `sheet_xlsx.cpp`, `sheet_ods.cpp`, `backend_native_model_m3d.cpp`) | **Done** | [MINIZ_REMOVAL_PLAN.md](MINIZ_REMOVAL_PLAN.md) -- replaced by `src/deflate.h`/`.cpp` + `src/zip_archive.h`/`.cpp` |
| tree-sitter (+ grammars) | syntax highlighting/parsing (`treesitter.cpp`) | Keep | [TREE_SITTER_REMOVAL_PLAN.md](TREE_SITTER_REMOVAL_PLAN.md) |
| PDFium | PDF viewer backend (`pdf_doc.cpp`) | Keep | [PDFIUM_REMOVAL_PLAN.md](PDFIUM_REMOVAL_PLAN.md) |
| GLFW | window/GL-context/input platform under `gfx::backend_native` | **Candidate** (large) | [GLFW_REMOVAL_PLAN.md](GLFW_REMOVAL_PLAN.md) |
| OpenSSL | WebSocket TLS (`wss://`) + handshake SHA-1 (`collab_websocket.cpp`) | Keep | [OPENSSL_REMOVAL_PLAN.md](OPENSSL_REMOVAL_PLAN.md) |
| ~~stb_truetype~~ (vendored) | font rasterization (`gfx/backend_native_text.cpp`) | **Done** | [STB_TRUETYPE_REMOVAL_PLAN.md](STB_TRUETYPE_REMOVAL_PLAN.md) -- replaced by `gfx/truetype.h`/`.cpp`, an in-house TrueType parser/rasterizer |
| ~~stb_image / stb_image_write~~ (vendored) | image decode (PNG/JPEG/BMP/GIF) + PNG encode | **Done** | [STB_IMAGE_REMOVAL_PLAN.md](STB_IMAGE_REMOVAL_PLAN.md) -- replaced by `src/png_codec.h`/`.cpp`, `src/jpeg_codec.h`/`.cpp`, `src/bmp_codec.h`/`.cpp`, `src/gif_codec.h`/`.cpp`, `src/image_codec.h`/`.cpp` |
| ~~miniaudio~~ (vendored) | `<audio>`/`<video>` element playback (`gfx/backend_native_audio.cpp`) | **Done** | [MINIAUDIO_REMOVAL_PLAN.md](MINIAUDIO_REMOVAL_PLAN.md) -- replaced by an in-house ALSA backend |
| Deno + webview_deno | optional `just run-wasm` launcher window (not linked into native `mep`) | Keep | [WEBVIEW_DENO_REMOVAL_PLAN.md](WEBVIEW_DENO_REMOVAL_PLAN.md) |

## Recommended order

Smallest/lowest-risk first, so each finished plan builds confidence (and,
in the DEFLATE case, reusable code) for the next one:

1. ~~**miniaudio**~~ -- **done.** Trivial usage surface (one `ma_engine` +
   a handful of `ma_sound` calls), and usage turned out to be WAV-only in
   practice, with decode already covered by the existing `wav_doc.cpp` --
   only a real ALSA output backend was genuinely new work. See
   `MINIAUDIO_REMOVAL_PLAN.md` for the full writeup.
2. ~~**stb_truetype**~~ -- **done.** Self-contained, well-understood after
   the alignment-bug fix already had us deep in `LoadFontData`. One real
   surprise along the way: the symbol fallback font turned out to be
   CFF-flavored, not TrueType -- resolved by re-subsetting it from
   Google Fonts' TrueType release instead of adding a second outline
   format's interpreter. See `STB_TRUETYPE_REMOVAL_PLAN.md` for the full
   writeup.
3. ~~**pugixml**~~ -- **done.** Narrow API surface, no XPath -- and a
   fuller consumer survey (prompted by build errors, not the original
   plan) found 2 more real consumers beyond the 3 originally scoped
   (`sheet_xlsx.cpp`/`sheet_ods.cpp`, XLSX/ODS import/export), all 5
   retargeted with the same mechanical swap. See
   `PUGIXML_REMOVAL_PLAN.md` for the full writeup, including a real
   scare during verification that turned out to be test-methodology
   (racing `:w` against still-draining queued keystrokes), not a code
   bug in either the old or new XML backend.
4. ~~**miniz**~~ -- **done.** Wrote the in-house DEFLATE/inflate codec
   (`src/deflate.h`/`.cpp`, both raw and zlib-wrapped) + ZIP container
   reader/writer (`src/zip_archive.h`/`.cpp`), verified against real
   Word/LibreOffice-produced `.docx`/`.odt` files and Python's `zlib` as
   an independent oracle throughout. Picked up 2 more consumers beyond
   the original 2-file scope (`sheet_xlsx.cpp`/`sheet_ods.cpp`, sharing
   `office_doc.cpp`'s ZIP helpers) plus the M3D importer's zlib-wrapped
   inflate, exactly as this plan anticipated. See
   `MINIZ_REMOVAL_PLAN.md` for the full writeup.
5. ~~**stb_image / stb_image_write**~~ -- **done.** PNG decode/encode
   reused miniz's DEFLATE codec exactly as planned
   (`deflate::InflateZlib`/`DeflateZlib`). JPEG needed the largest new
   chunk of code this whole dependency-removal effort has produced so
   far (marker parsing, JPEG-specific Huffman tables, MCU entropy
   decode, IDCT, chroma upsampling, YCbCr->RGB) -- caught and fixed one
   real bug during verification (quantization tables are transmitted in
   zigzag order, not natural order). BMP picked up a real R/B-channel-
   swap bug the same way, and ended up supporting `BI_BITFIELDS` (not
   just the originally-scoped `BI_RGB`) after finding that's what
   ImageMagick's own default 32-bit-with-alpha export actually writes.
   GIF's LZW decoder matched an independent decoder byte-for-byte on
   every fixture including transparency and interlacing. See
   `STB_IMAGE_REMOVAL_PLAN.md` for the full writeup.
6. **GLFW** -- by far the largest (real per-platform window/input/GL-
   context backends), deliberately last. Genuinely optional: nothing
   else here depends on it going away, and it's the same shape of
   project raylib was.

Lua, tree-sitter, PDFium, OpenSSL, and Deno/webview_deno are **not**
queued for removal -- see each one's own plan file for why (short
version: reimplementing a language VM, an incremental-parsing engine, a
PDF renderer, or TLS in-house is a bad trade of enormous effort/risk for
little to no benefit, not a "too hard to ever attempt" dodge).

## Shared verification approach

Every candidate's plan uses the same loop, established during the raylib
work:

- `nix develop --command cmake --build build/native -j$(nproc)` after
  each phase (see `env_nix_develop_build` memory -- toolchain only
  resolves inside `nix develop`).
- `nix develop --command just test` for the pure-logic regression suite
  (html-doc, org-doc, workspace, model3d-doc, collab-crdt; the
  `collab-session-test` failure with no args is pre-existing/unrelated,
  confirmed against a clean baseline -- not a regression signal).
- Live visual/behavioral verification against a real running `mep`
  instance: this sandbox has a working GPU and GUI now (`env_no_gpu_xvfb`
  is superseded), driven either through the `mep-agent` MCP tools when
  connected, or directly over its agent-control Unix socket
  (`~/.local/share/mep/agent-sockets/<pid>.sock`, JSON-RPC 2.0 with
  `Content-Length` framing, see `live_mep_instance_testing` memory) when
  it isn't -- `ui.screenshot`, `ui.key_press`/`key_down`/`key_up`,
  `command.run` for ex commands. Screenshot-compare before/after each
  phase touching rendering; for pure data-format phases (pugixml/miniz's
  parse/zip correctness), round-trip a real sample file instead
  (`office_doc.cpp`'s existing DOCX/ODT test fixtures if any exist, else
  a hand-crafted minimal `.docx`/`.odt`).
- Kill any test `mep` instance spawned for verification when done with
  it, and never save into a real user file from a scratch/test buffer
  (see the terminal-bug fix session's near-misses for why this is called
  out explicitly).
