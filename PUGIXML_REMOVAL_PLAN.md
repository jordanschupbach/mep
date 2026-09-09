# pugixml Removal Plan

**Status: done.** Replaced the `pugixml` `FetchContent` dependency
(`CMakeLists.txt`, fetched from a GitHub release tarball with its own
CMake build) with an in-house minimal XML DOM parser + writer
(`src/xml_doc.h`/`.cpp`, `xml::` namespace), used by `src/office_doc.cpp`,
`src/office_odt.cpp`, and `src/doc_export.cpp` for DOCX/ODT's XML parts --
plus, discovered only once work was underway, `src/sheet_xlsx.cpp` and
`src/sheet_ods.cpp` for XLSX/ODS spreadsheet import/export (the same ZIP+
XML shape; this plan's original survey missed them, see Phase 3).

**How to resume:** check the boxes below, `git log --oneline -- src/office_doc.cpp src/office_odt.cpp src/doc_export.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> `nix develop --command just test` -> round-trip a real
sample file -> tick the box -> next phase, pausing for the user's go-
ahead unless told to keep going.

---

## Current usage (grounded in reading all three consumers)

The API surface actually used, across `office_doc.cpp`, `office_odt.cpp`,
and `doc_export.cpp` combined, is narrow:

**Parsing (read path):**
- `pugi::xml_document` + `.load_buffer(data, size)` -- parse from an
  in-memory byte range (the ZIP-extracted `word/document.xml`/
  `content.xml`/`styles.xml` bytes; see `MINIZ_REMOVAL_PLAN.md` for
  where those bytes come from).
- Tree navigation: `.child("name")`, `.children("name")` (iterate
  same-named children), `.first_child()`, `.next_sibling()`, `.name()`.
- `.attribute("name")` -> `pugi::xml_attribute`, read via its implicit
  string/bool conversion (`OoxmlBoolOn`'s `.attribute("w:val")` pattern).
- `.text()` -> concatenated text content of a node (`.text().get()`).

**Writing (export path, `doc_export.cpp` + the DOCX-patch path in
`office_doc.cpp` around line 837 on):**
- `.append_child("name")` / `.append_attribute("name")` to build new
  elements (constructing DOCX `<w:pPr>`/`<w:r>`/`<w:rPr>`/... runs and
  ODT equivalents from mep's own `DocParagraph`/`DocTable` model).
- `.set_value(...)` on an appended attribute.
- `xml_document::save(std::ostream&, indent_string, pugi::format_raw)` --
  serialize the whole tree back to text (`office_doc.cpp:1139-1205`, via
  an `std::ostringstream`), which then becomes a ZIP entry's bytes
  (`WriteZipReplacingEntry`'s `new_content` parameter).

**Not used anywhere in the codebase** (confirmed by grep across all three
files): XPath (`select_nodes`/`select_single_node`), DTD/schema
validation, `pugi::xml_writer` custom sinks (the ostream overload covers
every real call site), CDATA/comment/processing-instruction node
handling, namespace-aware APIs (`w:`/`style:`/... prefixes are handled as
plain opaque tag-name strings throughout mep's own code, not resolved via
pugixml's namespace features -- simpler for an in-house parser too).

## Scoping decisions

1. **Element/attribute/text nodes only** -- no CDATA, comments, or
   processing instructions, since nothing in DOCX's `word/document.xml`
   or ODT's `content.xml`/`styles.xml`/`meta.xml`/`manifest.xml` (the
   only XML parts mep ever reads or writes -- confirmed by grepping for
   every `.xml` entry name referenced in `office_doc.cpp`/`office_odt.cpp`/
   `doc_export.cpp`) needs them. If a real-world file ever does contain
   one, the parser should skip it gracefully (matching pugixml's own
   default `parse_full` vs. minimal flag sets -- decide the exact
   tolerance during Phase 1 by testing against real-world Word/LibreOffice
   output, not just hand-crafted fixtures).
2. **No namespace resolution** -- treat `w:pPr`/`style:name`/etc. as
   opaque strings, matching every existing call site's own usage pattern.
   This sidesteps a real chunk of XML-spec complexity (namespace URI
   binding, prefix redeclaration scoping) that mep's own code never
   actually relies on.
3. **UTF-8 only.** DOCX/ODT XML parts are UTF-8 by the OOXML/ODF specs
   themselves (an explicit `encoding="UTF-8"` XML declaration is standard,
   and nothing in mep's own code handles any other declared encoding
   today -- `pugixml`'s own encoding-detection/transcoding machinery,
   which handles UTF-16/Latin-1/etc., is dead weight for this use case).
4. **mep already has a structurally similar in-house parser to crib
   from**: `html_doc.cpp`'s HTML parser (tag/attribute tokenizing, a DOM
   tree of nodes) solves the same *class* of problem (SGML-family markup
   -> tree) at a harder level (HTML's error-recovery rules are far
   messier than well-formed XML's strict rules) -- reuse its general
   shape (tokenizer -> tree builder), not its code directly (HTML and
   XML have different-enough grammars that sharing the same parser isn't
   appropriate).

## Phases

### Phase 1: real-world fixture gathering
- [x] Generated real fixtures via actual LibreOffice (`libreoffice
  --headless --convert-to`), not hand-crafted XML: an HTML source with
  headings, bold/italic/underline, a bulleted list, a table, `&amp;`/
  `&lt;` entities, a literal non-ASCII em-dash, and a self-closing
  `<br/>`, converted to both `.odt` and `.docx`. **Verified via:**
  inspecting the extracted `word/document.xml`/`content.xml` directly --
  confirmed real-world quirks to design against: only `&amp;`/`&lt;`
  entities appear in practice (no numeric char refs -- LibreOffice
  writes literal UTF-8 instead), double-quote attributes only, no CDATA/
  comments in any part, but very long attribute lists on the root
  element (a real stress case for the attribute parser) and genuine
  multi-byte UTF-8 passthrough in text content.

### Phase 2: minimal XML parser
- [x] `src/xml_doc.h`/`.cpp`, in a `xml::` namespace (not `XmlNode`/free
  functions as originally sketched -- see Phase 3's own note on why the
  pugixml-shaped handle API won this instead). Tokenizer handles open/
  close/self-closing tags, `"`/`'`-quoted attributes, the 5 standard
  entities plus numeric character references (UTF-8 encoded), CDATA
  sections (parsed into real `node_cdata` nodes, not skipped -- see
  Phase 3's finding that `office_odt.cpp`'s own read path already
  special-cases them), comments (skipped), and mixed content (mep's own
  paragraph-walking code distinguishes sibling text/element children as
  it walks, exactly as planned).
- [x] `load_buffer(data, size, flags, encoding)` matching pugixml's own
  call shape exactly (flags/encoding accepted, ignored -- this parser
  has one behavior) -- see Phase 3's note on why matching the shape
  exactly, not just loosely, was the right call.

### Phase 3: tree navigation API
- [x] Implemented `child`/`children()`/`children(name)`/`first_child`/
  `next_sibling`/`name`/`value`/`type`/`attribute`/`text` as pugixml-
  shaped lightweight handles (`xml::xml_node`/`xml_attribute`/`xml_text`,
  copyable, null-safe via `operator bool`) rather than the originally-
  sketched free-standing `XmlNode` tree type. **Verified via:** an
  exhaustive grep-based API survey across *all 5* real consumers (this
  plan's original survey only found 3 -- `sheet_xlsx.cpp` and
  `sheet_ods.cpp` also use pugixml, for XLSX/ODS spreadsheet import/
  export, found only once the mechanical `pugi::` -> `xml::` retarget
  turned up unconverted references). That fuller survey found real API
  surface beyond the original plan's list: a no-arg `children()`
  (iterate *all* children, not just same-named ones -- `office_doc.cpp`'s
  `AppendDocxRunText`), `xml_node::value()` (the raw text of a node that
  IS itself a text node, distinct from `.text()`'s "first text child of
  an element" -- `office_odt.cpp`'s `CollectOdtInline`), `.type()` +
  `node_cdata`/`node_pcdata` comparison, `xml_text::set()`, numeric
  `xml_attribute::set_value(int)`/`(unsigned int)` overloads,
  `insert_child_before()`, `xml_document::reset()`, and a real
  `xml_parse_result` (bool-convertible + `.description()`, not a bare
  `bool`) since the spreadsheet importers build "malformed X: ..." error
  messages from a failed parse. Matching pugixml's actual handle-based
  design this closely (rather than the plan's original free-function
  sketch) is exactly what let every one of the 5 consumers keep its
  reading logic **completely unchanged** beyond a mechanical `sed
  's/pugi::/xml::/g'`.

### Phase 4: tree construction + serialization (writer)
- [x] `append_child(name)` / `append_child(node_pcdata)` / `append_child(
  node_declaration)` / `append_attribute(name).set_value(...)` -- all
  three node-construction shapes real call sites use, matched exactly
  (the `node_declaration` case was a real Phase-1-survey gap too: found
  only once the fuller 5-file grep turned up `doc_export.cpp` building
  a `<?xml version="1.0" encoding="UTF-8"?>` declaration by hand for
  freshly-generated ODT parts).
- [x] `save(std::ostream&, indent, format_raw)`: emits well-formed XML
  with attribute-value escaping (`<`/`&`/`"`) and text-content escaping
  (`<`/`&`), no pretty-printing -- matches `pugi::format_raw`'s behavior,
  the only mode any real call site requests.

### Phase 5: swap-in + verification
- [x] Retargeted all 5 consumers (`office_doc.cpp`, `office_odt.cpp`,
  `doc_export.cpp`, `sheet_xlsx.cpp`, `sheet_ods.cpp`) via a mechanical
  `#include "pugixml.hpp"` -> `#include "xml_doc.h"` +
  `sed 's/pugi::/xml::/g'`, then fixed the handful of real gaps that
  build errors turned up (see Phase 3's note) -- confirming the "minimal
  call-site changes" goal held in practice, not just in the plan's
  intent.
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  clean, zero warnings under mep's own `-Werror` strict flags.
  `nix develop --command just test` -- pure-logic suite passes (same
  pre-existing/unrelated `mep-collab-session-test` gap as every plan in
  this series).
- [x] Round-trip test against the real LibreOffice-generated `.docx`/
  `.odt` fixtures: opened in mep (content matched a saved pugixml-era
  "before" screenshot pixel-for-pixel), edited, saved, and **independently
  verified with real LibreOffice** (`--convert-to txt`, a process mep
  never touches) -- exact content preserved both times, including the
  `&`/`<` entities, the table, and the edit itself. `unzip -t` also
  confirmed both saved archives are structurally valid ZIPs.
  One real scare along the way: an ODT edit round-trip first appeared to
  silently drop characters near the insertion point. Isolated against an
  unmodified pugixml-era build (via a disposable `git worktree`, after a
  `git stash` detour that briefly reverted the whole session's
  uncommitted work and had to be popped back immediately -- worktree is
  the safe way to do this kind of isolation test, not stash) -- the
  *original* pugixml-based mep lost characters in the same scenario, and
  more of them. Root cause was test methodology, not either XML backend:
  `ui.type_text`'s keystrokes are queued and drained over subsequent
  frames (documented in `main.cpp`'s own RPC comment), and firing
  `:w` immediately after without letting that queue drain raced the
  save against still-in-flight typing. A retest with proper waits
  between steps round-tripped perfectly. Recorded here since it cost
  real verification time and is a trap any future live-instance RPC
  test in this codebase should watch for.
- [x] Removed `FetchContent_Declare(pugixml)`/`FetchContent_MakeAvailable(pugixml)`
  and every `pugixml` reference from `CMakeLists.txt`'s
  `target_link_libraries` (`mep_core`, `mep-amalgam`) and strict-flags
  comment, and from `flake.nix`'s `pugixmlSrc`/
  `FETCHCONTENT_SOURCE_DIR_PUGIXML`; deleted
  `third_party_licenses/pugixml-LICENSE.txt`. Full clean reconfigure +
  rebuild from an empty `build/native` confirmed pugixml is never
  fetched at all anymore, not just unlinked.

## Non-goals

- XPath -- unused anywhere in mep's own code.
- Namespace-aware APIs -- unused; every call site treats prefixed names
  as opaque strings (Scoping decision 2).
- DTD/schema validation, external entity resolution -- unused, and
  external entity resolution in particular is a security liability (XXE)
  worth deliberately *not* adding even if some hypothetical future need
  arose; parse only what's inline.
- Streaming/SAX-style parsing -- every current use case loads a small,
  fully-buffered XML part (extracted from a ZIP entry already fully in
  memory) and builds a full DOM; no need for incremental/streaming parse.
