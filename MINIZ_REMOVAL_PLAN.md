# miniz Removal Plan

**Status: done.** Replaced the vendored `miniz` `FetchContent` dependency
(single `miniz.c`/`.h` pair, built as a static lib in `CMakeLists.txt`)
with an in-house DEFLATE codec (`src/deflate.h`/`.cpp`) + ZIP container
reader/writer (`src/zip_archive.h`/`.cpp`), used by `src/office_doc.cpp`/
`src/office_odt.cpp`/`src/doc_export.cpp` (DOCX/ODT read/write/export),
`src/sheet_xlsx.cpp`/`src/sheet_ods.cpp` (XLSX/ODS, sharing
`office_doc.cpp`'s `ReadZipEntry`/`WriteZipReplacingEntr*` -- found once
work was underway, same pattern as `PUGIXML_REMOVAL_PLAN.md`'s missed
consumers), and `gfx/backend_native_model_m3d.cpp` (M3D's zlib-wrapped
compressed body).

**How to resume:** check the boxes below, `git log --oneline -- src/office_doc.cpp src/doc_export.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> `nix develop --command just test` -> round-trip a real
sample file -> tick the box -> next phase, pausing for the user's go-
ahead unless told to keep going.

---

## Current usage -- and a head start already in the codebase

Reading `office_doc.cpp` and `doc_export.cpp` closely turns up something
worth knowing before scoping this: **`doc_export.cpp`'s ODT writer
(`BuildZipArchive`) already hand-writes the entire ZIP *container* format
itself** -- local file headers, central directory, end-of-central-
directory record, `PK\x03\x04` signatures, all field layout -- and only
reaches into miniz for two low-level primitives: `mz_crc32` (checksum) and
`tdefl_compress_mem_to_heap` (raw DEFLATE compression, no ZIP framing).
Its own comment explains why: miniz's *own* `mz_zip_writer_*` API was
tried first and produced archives real LibreOffice rejected
("BrokenPackageRequest") because it emits a data-descriptor-based local
header (general-purpose bit 3 set) that a real round-trip through Word/
LibreOffice doesn't tolerate -- confirmed both by the LibreOffice failure
and by cross-checking against Python's stdlib `zipfile` output. So the
ZIP *container* logic this plan needs for writing is **already written
and already proven against real-world tooling** -- what's missing is
purely the compression algorithm underneath it.

The remaining actual miniz surface, precisely:

- **CRC-32** (`mz_crc32`) -- used directly by `doc_export.cpp` already;
  trivial, well-known table-driven algorithm (~20 lines).
- **DEFLATE encode** (`tdefl_compress_mem_to_heap`,
  `tdefl_create_comp_flags_from_zip_params`) -- used directly by
  `doc_export.cpp`, and indirectly by `office_doc.cpp`'s
  `mz_zip_writer_add_mem` (DOCX write path).
- **DEFLATE decode** (inflate) -- not called directly anywhere in mep's
  own code, but required internally by `mz_zip_reader_extract_to_heap`
  (`office_doc.cpp`'s DOCX/ODT *read* path -- real Word/LibreOffice-
  produced `.docx`/`.odt` files use DEFLATE-compressed entries, not
  STORED, so this is a real requirement, not a theoretical one).
- **ZIP container *reading*** (`mz_zip_reader_init_mem`/
  `_locate_file`/`_file_stat`/`_get_num_files`/`_extract_to_heap`/`_end`)
  -- `office_doc.cpp`'s only remaining real gap; the writer side doesn't
  need new container-format work (see below).
- **ZIP container *writing*** (`mz_zip_writer_init_heap`/`_add_mem`/
  `_add_mem_ex_v2`/`_add_from_zip_reader`/`_finalize_heap_archive`/`_end`,
  used by `office_doc.cpp`'s `WriteZipReplacingEntry`) -- this plan's
  recommended approach is to **stop using miniz's writer API entirely
  and reuse `doc_export.cpp`'s own already-in-house, already-real-world-
  verified `BuildZipArchive`** (read all entries via the new reader,
  splice in the replacement, rebuild) instead of also porting miniz's
  writer semantics. One less thing to write, and it collapses two
  slightly-different ZIP-writing code paths in the codebase into one.

## Scoping decisions

1. **DEFLATE only** (method 8) plus STORED (method 0) passthrough --
   matches every real entry mep's own writer produces and every entry
   real-world DOCX/ODT tooling produces. No other ZIP compression method
   (BZIP2, LZMA, etc.) is used by OOXML/ODF or by mep's own writer.
2. **No streaming** -- every current call site fully buffers both the
   ZIP bytes and each extracted entry in memory already (`std::vector<unsigned char>`,
   `std::string`); an in-house codec operating on flat in-memory
   buffers (not a streaming/chunked API) matches this exactly and is
   simpler to write and verify.
3. **Share the codec with `STB_IMAGE_REMOVAL_PLAN.md` -- and with
   `gfx/backend_native_model_m3d.cpp`, a third consumer found while
   researching this plan.** PNG's `IDAT` chunks are zlib-format DEFLATE
   streams -- literally the same compressed-block format (RFC 1951)
   miniz uses for ZIP entries, just wrapped in a 2-byte zlib header/
   4-byte Adler-32 trailer (RFC 1950) instead of ZIP's framing. The M3D
   3D-model importer (`backend_native_model_m3d.cpp:185`) already calls
   miniz directly too, for the exact same reason:
   `tinfl_decompress_mem_to_heap(..., TINFL_FLAG_PARSE_ZLIB_HEADER)` --
   M3D's compressed mesh chunks use zlib framing, not ZIP's, so this is
   the *same* zlib-wrapped-DEFLATE case as PNG, not a third format to
   support. Writing the core DEFLATE encoder/decoder as a standalone,
   container-agnostic module (raw byte-in/byte-out, with both the ZIP
   raw-deflate entry point Phase 2/3 need and a thin zlib-header/Adler-32
   wrapper for the PNG/M3D case) means `STB_IMAGE_REMOVAL_PLAN.md`'s PNG
   phase *and* the M3D importer both reuse it instead of each
   implementing DEFLATE separately -- this is why `DEPENDENCIES.md`'s
   recommended order does miniz before stb_image, and why Phase 6 below
   updates M3D's call site too, not just `office_doc.cpp`/`doc_export.cpp`.
4. **No ZIP writing beyond what mep needs to produce** -- no support for
   multi-disk archives, ZIP64 (files this small never need it),
   encryption, or arbitrary compression-level tuning beyond matching
   `MZ_DEFAULT_LEVEL`'s current behavior.

## Phases

### Phase 1: CRC-32
- [x] Standard table-driven CRC-32 in `src/deflate.cpp` (`deflate::Crc32`).
  **Verified via:** the standard check value, `crc32("123456789") ==
  0xCBF43926`, passes.

### Phase 2: DEFLATE decode (inflate)
- [x] RFC 1951 inflate in `src/deflate.cpp` (`deflate::InflateRaw`):
  fixed and dynamic Huffman blocks (canonical Huffman construction
  shared by both, following the well-known "puff.c" structure -- a
  count[]/symbol[] decode table + a bit-by-bit `DecodeSymbol` walk, not
  copied from puff.c but the same standard algorithm), stored blocks,
  the length/distance extra-bit tables, sliding-window back-reference
  copy.
- [x] **Verified via:** 8 real DEFLATE streams generated by Python's
  `zlib.compressobj(level, DEFLATED, -15)` (raw deflate, no wrapper) --
  small text, repeated XML-like markup, full-byte-range binary, empty
  input, a single byte, highly repetitive data (dynamic Huffman with
  long runs), `os.urandom` (incompressible, exercises the "grows"
  case), and an explicit level-0 stored-block stream -- every one
  decoded byte-identical to the original via a standalone test program
  (not the real build; a throwaway `nix develop`-compiled binary, same
  methodology as the raylib/stb_truetype work). This is an independent
  oracle in the strict sense the plan asked for: Python's `zlib` module
  is CPython's own binding to the real system zlib, sharing zero code
  with this implementation.

### Phase 3: DEFLATE encode (deflate)
- [x] `deflate::DeflateRaw` in `src/deflate.cpp`: a hash-chain LZ77
  match finder (classic zlib-style head[]/prev[] chain structure,
  bounded 64-candidate search depth, greedy longest-match -- no lazy
  one-byte-ahead lookahead) emitting **fixed Huffman blocks only** (one
  block per call, `BFINAL=1` immediately) -- see this file's own updated
  top comment for why fixed-only was the right simplification: RFC 1951
  permits it, every real inflate implementation (including Phase 2's own
  and real-world Word/LibreOffice) decodes it identically to dynamic
  Huffman, and it avoids needing to build/transmit a dynamic Huffman
  table on the encode side, at a real but acceptable compression-ratio
  cost for mep's small XML/text payloads (confirmed empirically: this
  encoder's output on the same 8 test payloads ranged from matching
  Python's dynamic-Huffman `zlib` closely on structured text to
  noticeably larger on highly repetitive data -- e.g. 5000 bytes of
  `'A'` compressed to 36 bytes here vs. 23 with dynamic Huffman -- both
  well within what `BuildArchive`'s own store-if-it-doesn't-shrink
  fallback already handles gracefully).
- [x] **Verified via:** all 8 Phase 2 test payloads encoded then (a)
  decoded correctly by this same module's own `InflateRaw` and (b)
  independently decoded correctly by Python's `zlib.decompress(data,
  -15)` -- confirming the encoder's output is genuinely standard-
  compliant DEFLATE, not just self-consistent with this one codebase's
  own decoder.

### Phase 4: ZIP container reader
- [x] `src/zip_archive.cpp`'s `zip::Extract`/`zip::ListAll`: EOCD located
  by backward search for `PK\x05\x06` (handling the variable-length
  trailing comment field), central directory walked for `PK\x01\x02`
  entries, extraction reads the *local* header only for its name/extra-
  field lengths (to locate where entry data starts) while always
  trusting the *central directory's* copy of CRC/sizes -- deliberately
  robust against a data-descriptor-writing tool (general-purpose bit 3
  set, local header's own crc/size left zero), which real-world Word/
  LibreOffice output sometimes uses even though `BuildArchive` itself
  never does.
- [x] **Verified via:** real fixtures, not synthetic ones --
  `zip::ListAll` against an actual Word-produced `.docx` and an actual
  LibreOffice-produced `.odt` (reused from `PUGIXML_REMOVAL_PLAN.md`'s
  Phase 1) extracted every single entry correctly (11 DOCX parts, 13 ODT
  parts including empty stored directory placeholders and the stored-
  not-deflated `mimetype` entry), each individually cross-checked
  byte-identical against `unzip -p`'s own extraction as a second
  independent oracle.

### Phase 5: swap-in + consolidate the writer
- [x] `office_doc.cpp`'s `ReadZipEntry` now a thin wrapper over
  `zip::Extract`.
- [x] `office_doc.cpp`'s `WriteZipReplacingEntry`/`WriteZipReplacingEntries`
  rewritten on top of `zip::ListAll` + `zip::BuildArchive` (moved from
  `doc_export.cpp` into the new shared `zip_archive.h`/`.cpp` unchanged
  in behavior, per this plan's own recommendation) -- confirmed no ODT-
  specific assumptions needed generalizing, since `BuildArchive` was
  already format-agnostic (just takes a name/data/store list). One real
  behavior change from the miniz-based version, documented in
  `office_doc.h`'s own updated comment: unchanged entries are now
  decompressed-then-recompressed rather than raw-copied at the
  compressed-byte level (miniz's `mz_zip_writer_add_from_zip_reader`) --
  content-correctness is identical either way, and matching the
  original compressed bytes exactly was never a goal (Non-goals).
- [x] `doc_export.cpp`'s own `mz_crc32`/`tdefl_compress_mem_to_heap`
  calls retargeted onto `deflate::Crc32`/`DeflateRaw` (via
  `zip::BuildArchive`, which now does this internally -- `doc_export.cpp`
  no longer calls a compression primitive directly at all, just builds
  an entry list and calls `zip::BuildArchive`).
- [x] `#include "miniz.h"` removed from both files (and from
  `sheet_xlsx.cpp`/`sheet_ods.cpp`, which never included it directly but
  are covered by the same `ReadZipEntry`/`WriteZipReplacingEntries` API).

### Phase 6: cleanup + verification
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  clean, zero warnings under mep's own `-Werror` strict flags (a few
  real `-Wsign-conversion` hits during standalone-module development,
  fixed before ever reaching the full build). `nix develop --command
  just test` -- pure-logic suite passes (same pre-existing/unrelated
  `mep-collab-session-test` gap as every plan in this series).
- [x] Full read+write+re-verify round-trip against real fixtures, both
  formats: opened a real Word-produced `.docx` and LibreOffice-produced
  `.odt`, edited, saved, then independently verified with `unzip -t`
  (archive integrity) and real LibreOffice -- **including the exact
  `libreoffice --headless --convert-to pdf` check `doc_export.cpp`'s own
  comment describes** as the original "BrokenPackageRequest" failure
  mode this whole hand-written-ZIP-writer architecture was built to
  avoid. It succeeded cleanly (a real, valid PDF produced), confirming
  the in-house writer's local-header format (no data descriptor, real
  sizes/CRC inline) is preserved correctly through the miniz -> in-house
  DEFLATE swap. Text content verified exact via `--convert-to txt` for
  both formats, including the edit itself, the `&`/`<` entities, and the
  table.
- [x] `backend_native_model_m3d.cpp`'s `tinfl_decompress_mem_to_heap(...,
  TINFL_FLAG_PARSE_ZLIB_HEADER)` retargeted onto `deflate::InflateZlib`,
  same zlib-wrapped decode entry point PNG will use -- preserving the
  "one shared codec, not a second copy" property the old miniz-based
  code deliberately had. **Verified via:** no `.m3d` test fixture exists
  in-tree (M3D's own dedicated smoke test doesn't exercise real file
  loading -- see `CMakeLists.txt`'s own comment on why), so this used a
  targeted standalone test instead: a real zlib-compressed buffer
  produced by Python's `zlib.compress` (same RFC 1950 framing
  `TINFL_FLAG_PARSE_ZLIB_HEADER` expected), fed through
  `deflate::InflateZlib` directly, confirmed to decode byte-identical to
  the original and correctly begin with the `HEAD` chunk magic
  `ChunkIs(inflated_storage, 0, "HEAD")` checks for -- the exact
  condition the migrated code path depends on.
- [x] `FetchContent_Declare(miniz)`/`FetchContent_MakeAvailable(miniz)`
  and the `add_library(miniz STATIC ...)` block removed from
  `CMakeLists.txt`, along with every `target_link_libraries(... miniz ...)`
  (`mep_core`, `mep_add_gfx_native_smoke`'s targets, `mep-model3d-doc-test`,
  `mep-amalgam`) and `flake.nix`'s `minizSrc`/
  `FETCHCONTENT_SOURCE_DIR_MINIZ`. `src/deflate.cpp` added alongside
  `src/wav_doc.cpp` to the standalone gfx-smoke/model3d-doc-test targets
  (same "not part of `MEP_GFX_NATIVE_SOURCES` since it's also used
  outside the gfx:: backend" reasoning `MINIAUDIO_REMOVAL_PLAN.md`
  established for `wav_doc.cpp`). Full clean reconfigure + rebuild from
  an empty `build/native` confirmed miniz is never fetched at all
  anymore, not just unlinked.
- [x] Deleted `third_party_licenses/miniz-LICENSE.txt` (`git rm`) once
  `grep -rln "mz_\|tdefl_\|tinfl_\|miniz\.h\|MZ_"` across `src/` (incl.
  `src/gfx/`) returned nothing but stale comment mentions, all fixed for
  accuracy in `office_doc.h`/`doc_export.h`/`office_doc.cpp`.

## Non-goals

- ZIP64, multi-disk archives, encryption -- unused by any real DOCX/ODT
  file mep needs to handle.
- Matching miniz's exact compression ratio/speed -- correctness and
  "reasonably small," not byte-for-byte parity with miniz's own output.
- A general-purpose streaming ZIP API -- every current use case is
  small, fully-buffered files.
