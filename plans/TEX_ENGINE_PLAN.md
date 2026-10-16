# In-house TeX: engine, rasterizer and previews

Goal: mep compiles TeX and renders maths with no external programs.
tectonic and pdftoppm become *optional* system backends, chosen by a
setting, for when a document needs packages managed by a system TeX
installation. Nothing outside this repo (other projects, flake devShells)
should need to carry a TeX toolchain just so mep can render maths.

Status legend: `[ ]` not started, `[~]` in progress, `[x]` done.

---------------------------------------------------------------------------

## 1. Where TeX is used today

| Use | Call site | Pipeline |
| --- | --- | --- |
| Org/mepml maths previews | `mep_org_latex_render` (main.cpp, kBuiltinOrgLatex) | tectonic -> pdftoppm -> cropped PNG + `.base` |
| Presentation stand-in | `RenderMathFastPng` (main.cpp) | in-house typesetter (math_tex.cpp + layout in main.cpp) |
| HTML pages in the browser pane | `LayoutMathExpression` | in-house typesetter |
| org-babel `latex` blocks | babel language table (main.cpp ~19781) | tectonic -> pdftoppm onto `:file` |
| Org PDF export | main.cpp ~21724 | LaTeX export -> tectonic |
| mepml PDF / Beamer export | main.cpp ~26661, `mep-mepml` (mepml_tool.cpp) | LaTeX export -> tectonic |
| `.tex` Run button | main.cpp ~32305 | tectonic |
| Rmd / knitr PDF | main.cpp ~32453 | knitr -> tectonic |

## 2. Target architecture

```
                     ┌────────── tex_engine = builtin ──────────┐
 call sites ──▶ mep_tex_compile(tex, opts, cb) ──┤                                          ├──▶ PDF
                     └── tex_engine = system: tectonic / pdflatex / xelatex / lualatex / latexmk / argv

 PDF ──▶ mep_pdf_to_png(pdf, png, dpi, cb) ──▶ tex_rasterizer = builtin (mep's PDF renderer) | system (pdftoppm)
```

* **One entry point per job** (Lua, kBuiltinOrgLatex): every call site
  above goes through `mep_tex_compile` / `mep_pdf_to_png`, so the switch
  is honoured everywhere and the argv shapes of the system tools live in
  one table.
* **Settings** (settings panel, section "TeX"):
  * `tex_engine`: `builtin` | `system` (default `builtin` once the engine
    can run LaTeX; `system` until then).
  * `tex_system_command`: `tectonic` | `pdflatex` | `xelatex` | `lualatex`
    | `latexmk`, or any argv (`allow_custom`); `%f` = the .tex file,
    `%o` = the output directory.
  * `tex_rasterizer`: `builtin` | `system` (pdftoppm).
  * `tex_packages`: `bundle` | `bundle+system` -- whether the builtin
    engine also searches the system texmf tree (kpsewhich, `$TEXMF*`).
* **Engine** (`src/tex/`, library `mep_tex`, CLI `mep-tex`): TeX82 +
  e-TeX + the pdfTeX primitives LaTeX2e/expl3 need, writing PDF directly.
* **Bundle**: a curated, compressed TeX tree (LaTeX kernel, amsmath,
  amsfonts, standalone, graphics, xcolor, hyperref, geometry, beamer,
  Latin Modern + CM/AMS Type1 fonts, hyphenation patterns) embedded in
  the binary. LPPL/OFL/GUST-licensed files, redistributed unmodified,
  their licences listed in `third_party_licenses/`.

## 3. Phases

### Phase 1 -- the switch and the in-house rasterizer
- [x] `mep-tex pdf-to-png` (src/tex/tex_main.cpp, src/pdf_raster.cpp): one
      page by the PDF pane's own renderer; `mep.pdf_rasterize` in-process.
- [x] The command table in C++ (src/tex/tex_backend.cpp: tectonic,
      pdflatex, xelatex, lualatex, latexmk, typed argv with `%f`/`%o`),
      exposed as `mep.tex_command`; `mep.tex_compile` / `mep.pdf_to_png`
      and their argv forms in kBuiltinTex (main.cpp).
- [x] Every call site in section 1 routed through them; `mep-mepml`'s PDF
      export through the same table (`--tex-engine`, `--tex-command`,
      `$MEP_TEX_ENGINE`, `$MEP_TEX_COMMAND`).
- [x] Settings rows (`tex_engine`, `tex_system_command`, `tex_rasterizer`);
      `:TexInfo`; help/tex.org.
- [x] pdftoppm no longer required (default `tex_rasterizer = builtin`).
- [x] The a11y test's tagged-PDF round trip runs through mep-tex (phase 6).

### Phase 2 -- previews that look like TeX -- superseded
Previews go through the engine itself since phase 6 (standalone, cached
PNGs), so they are TeX's own output; the in-house maths layout stays as
the fallback when no engine runs. These items are kept for that fallback
only.
- [ ] Embed Latin Modern Math (OTF, CFF outlines; gfx/cff.cpp reads them).
- [ ] OpenType MATH table reader: constants, italic corrections, top
      accent attachment, glyph variants and assemblies for stretchy
      delimiters/radicals, kerning.
- [ ] Re-base the math layout (main.cpp `LayoutMath*`) on the MATH
      constants (TeXbook Appendix G rules, via the OpenType mapping).
- [ ] Text-face runs (`\text`, `\mathrm`) in Latin Modern Roman.
- [ ] Previews use the typesetter directly when `tex_engine = builtin`
      (no PDF round trip) until phase 6 routes them through the engine.

### Phase 3 -- engine core (TeX82 semantics) -- done
Knuth's tex.web, translated mechanically by `tools/web2cpp/` (WEB tangler,
Pascal parser, C++ emitter) into `src/tex/gen/`, with `src/tex/web/mep-tex.ch`
for the system parts (files through `tex_resolver`, capacities, the clock).
Regenerate with `tools/web2cpp/web2cpp.py third_party/tex/tex.web
src/tex/web/mep-tex.ch src/tex/web/mep-etex.ch src/tex/web/mep-pdftex.ch
--out src/tex/gen --members src/tex/tex_engine_members.h --externals
load_pool_strings`.
- [x] INITEX log, plain.fmt, and story.tex's DVI identical to Knuth's TeX.

### Phase 4 -- e-TeX, pdfTeX, LaTeX
e-TeX and pdfTeX are written from their manuals (etex_man, the pdfTeX
manual), not ported: etex.ch forbids modified copies, pdfTeX is GPL.
`mep-etex.ch`/`tex_etex.cpp` and `mep-pdftex.ch`/`tex_pdf.cpp` hook them in.
- [x] e-TeX: `\protected`, `\detokenize`, `\unexpanded`, `\numexpr` family,
      `\ifdefined`, `\ifcsname`, `\unless`, `\scantokens`, `\showtokens`,
      `\lastnodetype`, 32768 registers, mark classes, `\middle`, TeXXeT
      primitives. Test logs identical to `pdftex -ini -etex`.
- [ ] e-TeX remainder: `\lastlinefit`, `\savinghyphcodes`, TeXXeT
      reversal, `\tracingnesting`, `\showgroups` command text.
- [x] pdfTeX primitives (all of them parse; string/file/expansion ones
      give TeX Live's results): `\expanded`, `\pdfstrcmp`, escapes, dates,
      `\pdfmdfivesum`, `\pdfmatch`, `\pdfprimitive`, colour stacks, objects,
      annotations, links, destinations, outlines, forms, images ...
- [x] pdflatex.fmt builds; its log matches TeX Live's except banner lines
      and string/cs counts (the `\pdfprimitive` twins). A LaTeX+amsmath
      document's DVI is byte-identical to TeX Live's.
- [x] PDF backend (`tex_pdf_out.cpp`): pages, text with TJ kerning,
      rules, literals, colour stacks, `\pdfsave/\pdfsetmatrix/\pdfrestore`,
      forms, annotations, links (single line), destinations, outlines,
      named destinations, Type 1 fonts from `pdftex.map` + `.enc`,
      subset (CharStrings and Subrs), PNG (with alpha), JPEG, PDF pages
      (copied through mep's PDF parser). Pages render identically to
      TeX Live's pdflatex with poppler (article+amsmath; hyperref, colour,
      lmodern/T1, graphicx rotation, includegraphics).
- [x] Links broken across lines and pages (pdfTeX's link stack: a part
      per line, and the boxes at the same nesting level after a page
      break), rectangles identical to pdfTeX's.
- [x] Virtual fonts (`.vf` packets: characters, rules, specials, nested
      VFs; local fonts loaded as TeX fonts). The bundle carries psnfss's
      fonts (times, helvetic, courier, palatino, avantgar, bookman,
      ncntrsbk, zapfchan, mathpazo/fpl, charter, utopia, rsfs; 26 MB in
      all): every psnfss style compiles, Times and Palatino
      pixel-identical to pdflatex.
- [x] Micro-typography (`tex_microtype.cpp`, written from the manual and
      checked against pdfTeX as a black box): protrusion (margin kern
      nodes, `\lpcode`/`\rpcode`, `\pdfprotrudechars=2` in the line
      breaker, `\leftmarginkern`/`\rightmarginkern`) -- `\showbox`
      identical; font expansion (`\pdffontexpand`, `\efcode`, expanded
      font copies, the step nearest the line's need, `\pdfadjustspacing=2`
      counting font stretch/shrink in the breaker's widths, drawn with a
      scaled text matrix) -- the same breaks and `Tm` scales as pdfTeX;
      3 lines in 231 of a stress test pick a step one off.
- [x] `\letterspacefont` (quad-relative, own font parameters, glyphs
      centred) and `\pdfcopyfont`, taking `\global` like `\font`;
      microtype's `\textls` matches pdflatex.
- [x] `\pdfliteral shipout` (expanded when the page ships).
- [x] Font sizes in `Tf` to 4 decimals and `/Widths` to 0.1, as pdfTeX
      writes them, positions reckoned from those (TJ kerns as pdfTeX's
      but for rounding ties).
- [ ] Backend remainder: threads, PK/Type 3 fallback (needs METAFONT-made
      bitmaps the bundle does not have), object streams.
- [x] `mep-tex compile` (`tex_compile.cpp`): format chosen from the
      document, built once and cached in `~/.cache/mep/tex/formats/`
      (keyed by engine build + trees; old ones pruned), reruns while aux
      files change, TeX trees from `--tree`, `MEP_TEX_TREES`, kpsewhich
      or the usual paths (`ls-R` read instead of walking). The editor's
      builtin backend is enabled. A mepml export (tcolorbox/TikZ,
      hyperref, T1) is pixel-identical to TeX Live's pdflatex.
- [x] Runs: standalone (previews), beamer (mepml decks) -- see phase 5.

### Phase 5 -- bundle and resolver -- done
- [x] `tools/tex_bundle.py` packs a TeX Live tree's tex/, fonts (tfm,
      type1, enc, vf) and generated config into `tex-bundle.mtb` (deflate
      per file); drops map lines for fonts it does not carry; writes
      `mep-fallback.map` (EC/TS1 drawn by Latin Modern, instead of 58 MB of
      cm-super), `MANIFEST.json` and `LICENSES.txt`.
- [x] `nix/tex-bundle.nix` (flake `packages.tex-bundle`, installed as
      `share/mep/tex/tex-bundle.mtb`; `just tex-bundle` for dev builds):
      scheme-basic + lm, ec, tcolorbox/pgf, standalone, beamer, booktabs,
      caption, enumitem, mathtools, listings, siunitx, microtype ... --
      152 TeX Live packages, 21 MB.
- [x] `tex_bundle.cpp`: found beside mep, unpacked once into
      `~/.cache/mep/tex/bundle-*` with an ls-R; `tex_packages` /
      `--packages`: `bundle` | `bundle+system` | `system`.
- [x] Checked: maths preview (standalone), Beamer, the mepml article and
      decks; previews and Beamer pixel-identical to TeX Live's pdflatex
      on the same packages.

### Phase 6 -- switch over -- done
- [x] `tex_engine` default `builtin` (editor and `mep-mepml`); previews,
      exports, `.tex` Run, babel latex blocks, Rnw all go through the
      switch. `:TexInfo` says when mep-tex is missing.
- [x] Tagged PDF exports under mep-tex: it reads xdvipdfmx's `pdf:`
      specials (obj, stream, put, code, literal, content, docinfo, docview;
      `@name`, `@thispage`, `@catalog`), and the exporter turns tagging on
      for `\meptexversion` as for XeTeX. `mep-a11y-test`'s PDF round trip
      runs through mep-tex.
- [x] ToUnicode maps (`\pdfgentounicode`, `tfm:` entries), pdfTeX's
      own state (`\pdfglyphtounicode` ...) dumped in the format.
- [x] Exports under pdfTeX: common symbols, Greek and sub/superscript
      digits get a glyph, other characters a box and a warning, not an
      error (XeTeX took any character; pdfLaTeX stops).
- [x] Comparison harness `tools/tex_compare.py` (help/, examples/, the
      tflow docs, every maths fragment as a preview) against TeX Live's
      pdflatex with the bundle's packages + cm-super (`.reference` of
      nix/tex-bundle.nix): ~980 documents, none failing in mep-tex; the
      rest pixel-identical, or within 0.4% of pixels where T1 text is
      Latin Modern instead of cm-super.
- [x] Help (help/tex.org, mepml.org); flake devShell without tectonic.
- [x] Previews of amsmath displays: `align`, `multline`, `alignat`,
      `flalign` overflowed the preview's `varwidth=\maxdimen` line, and
      `equation`/`gather`/`align*` made 8000pt-wide pages (pdflatex the
      same). A display environment is now previewed as `\[ ... \]` around
      its inner form (aligned, alignedat, gathered), \tag shown after its
      line, \label/\nonumber dropped (kBuiltinOrgLatex).

### Later
- [ ] XeTeX-style OpenType fonts (fontspec) via a shaping layer.
- [ ] SyncTeX-style source/PDF mapping for the PDF pane.
