#ifndef MEP_DOC_EXPORT_H
#define MEP_DOC_EXPORT_H

#include <string>

// Both functions parse `html` via html_doc.h's own ParseHtml (a bare
// fragment or a full <html>/<body>-wrapped document, either tolerated
// the same way that parser always is) and walk the resulting DOM to
// synthesize a different document format entirely -- HTML is the shared
// intermediate representation the org exporter (kBuiltinOrgExport,
// main.cpp) already produces, so PDF/ODT export builds on that one DOM
// walk instead of a second/third from-scratch org-source walker per
// backend. Deliberately raylib-free (same reasoning as html_doc.h/
// office_doc.h/pdf_doc.h/sheet_doc.h): pure CPU-side generation, no GL
// context needed, usable/testable independent of the GUI.

// Renders `html`'s DOM as a complete LaTeX document (\documentclass
// through \end{document}) -- `title`/`author` may both be empty
// (\maketitle is only emitted when title is non-empty). `base_dir`
// resolves a local <img src="relative/path">, the same convention
// main.cpp's own HtmlLayoutCtx::base_dir uses for the in-pane browser --
// an image whose resolved path can't be read from disk is dropped with a
// comment left in its place rather than failing the whole export. A
// <math> node (html_doc.cpp's ExtractMathSpans) round-trips near-
// losslessly: its raw LaTeX source is already valid LaTeX math, just
// re-wrapped in $..$/\[..\] here. Always succeeds -- nothing about
// turning a DOM tree into LaTeX text can fail; a follow-on `tectonic`
// compile (kBuiltinOrgExport, main.cpp) is what can actually fail, and
// that's the caller's concern, not this function's.
std::string ExportHtmlToLatex(const std::string &html, const std::string &title, const std::string &author,
                               const std::string &base_dir);

// Same DOM walk, targeting a real .odt package -- built from scratch
// (mimetype/META-INF/manifest.xml/meta.xml/styles.xml/content.xml, plus
// one Pictures/imgN.<ext> zip entry per embedded local image), zipped via
// miniz the same way office_doc.cpp already writes docx/odt zip entries
// (mz_zip_writer_add_mem), just building every entry directly rather
// than patching one into an existing template archive -- and every style
// content.xml references is defined inline as an <office:automatic-
// style>, the same self-contained approach office_odt.cpp's own
// SaveOdtToMemory already uses (see GetOrCreateTextStyle/
// GetOrCreateParaStyle there), rather than depending on named styles
// resolving out of styles.xml. A <math> node has no real OpenDocument
// Formula (MathML) support here -- shown as its raw LaTeX source in
// monospace text instead (documented v1 scope cut, see this file's own
// .cpp). Returns true on success (writes `out_path`); false + a message
// in `error` otherwise (only a local file-write failure can actually
// happen here -- an unreadable/missing image is dropped with a text
// placeholder, same tolerance as the LaTeX backend, not a hard error).
bool ExportHtmlToOdt(const std::string &html, const std::string &out_path, const std::string &title,
                     const std::string &author, const std::string &base_dir, std::string &error);

#endif
