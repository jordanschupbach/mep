# PDFium Removal Plan

**Verdict: keep.** No phases below -- recorded in
[DEPENDENCIES.md](DEPENDENCIES.md)'s inventory for completeness, not
because removal is planned.

## What it's used for

`src/pdf_doc.cpp` uses PDFium (fetched as a prebuilt shared library from
`bblanchon/pdfium-binaries`, since upstream has no CMake build -- see
`CMakeLists.txt`'s own comment) as mep's entire PDF-viewer backend: page
rendering to a bitmap, text extraction, and navigation. Not available
under Emscripten (`pdf_doc.cpp`'s `#if defined(__EMSCRIPTEN__)` stub) --
already the narrowest footprint this dependency could have.

## Why not

PDF is a genuinely enormous file format spec: a page-description
language (content streams with its own operator set), multiple font
technologies embedded directly in the file (Type1, TrueType, CFF/
Type0/CID-keyed composite fonts), several independent color space
models (DeviceGray/RGB/CMYK, ICC profiles, Indexed, Separation),
a dozen-plus stream filters (FlateDecode, DCTDecode/JPEG, CCITTFaxDecode,
JBIG2Decode, LZWDecode, ASCII85/ASCIIHex, RunLength...), an object/xref
model with incremental updates, optional encryption (RC4/AES with
several revision levels), and PDFium itself is built on real
FreeType/HarfBuzz-class font shaping to render any of that correctly.
This isn't a "vendor a smaller decoder" situation the way stb_image is
for images -- there is no meaningfully scoped-down subset of "read and
render a PDF" the way `STBI_ONLY_PNG` narrows image decoding, because
even a single simple PDF routinely exercises several of the above at
once (a scanned PDF alone needs DCTDecode or CCITTFaxDecode plus a
color space plus the content-stream interpreter placing the image).

PDFium is itself Google's production PDF engine (what Chrome renders
PDFs with), built from a huge, heavily-fuzzed C++ codebase -- exactly
the kind of "don't reinvent this, the value is entirely in correctness
across real-world files you didn't author" dependency raylib was *not*:
raylib's own scope was small enough and general enough (draw a
rectangle, blit a texture, rasterize a glyph) that mep's specific needs
(no lighting model, custom glyph-atlas fast paths) were easy to match or
beat with an in-house backend. There's no equivalent "mep-specific
angle" on PDF rendering to gain by owning it.

## Why the prebuilt-binary tradeoff (vs. vendoring source) is fine as-is

`CMakeLists.txt`'s own comment already documents this: PDFium has no
CMake build (Google's own GN/ninja toolchain), so building from source
would mean either standing up GN/ninja as a second build system inside
mep's tree, or writing a from-scratch CMake build for a codebase this
large -- neither is worthwhile just to avoid a prebuilt-binary fetch, and
every bundled sub-dependency (freetype/lcms/openjpeg/zlib/libpng/
libjpeg-turbo/abseil/icu/simdutf/fast_float/llvm-libc) is already
permissively licensed (see `third_party_licenses/pdfium-LICENSE.txt`).

## If this ever gets revisited

If PDF support's scope ever narrowed dramatically (e.g. "render text-
only PDFs with no embedded fonts, no images, no encryption"), a from-
scratch minimal reader might become tractable -- but that would be a
product decision to drop PDF fidelity, not a dependency-removal project
in the raylib/GLFW/stb_* sense.
