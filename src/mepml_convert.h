#ifndef MEP_MEPML_CONVERT_H
#define MEP_MEPML_CONVERT_H

#include <string>
#include <vector>

#include "mepml_doc.h"

// Converting mepml documents to and from other formats.
//
// Export works from the parsed mepml::Document (src/mepml_export.cpp).
// Markdown, Org, RTF, plain text, DOCX and ODT are written directly; LaTeX
// goes through the document's HTML (mepml::ToHtml) and doc_export.h, and
// PDF is that LaTeX compiled by tectonic (the caller's job -- see
// kBuiltinMepml / mep-mepml). DOCX, ODT and RTF use named styles (Heading
// N, Title, Source Code, Quote, Caption, ...), and carry what they have no
// element for -- //? metadata, code block headers, bibliography fields --
// as custom document properties, with bookmarks marking where each code
// block starts. Markdown and Org carry the bibliography in a comment.
//
// Import produces mepml *text* (src/mepml_import.cpp): HTML through mep's
// DOM parser (html_doc.h); Markdown, Org and RTF through small readers of
// their own; DOCX and ODT read straight from their XML (styles, numbering,
// footnotes, fields, OMML/MathML maths as TeX), with mep's office document
// loaders (office_doc.h) as the fallback. Pictures inside DOCX/ODT/RTF are
// written out as files next to the result. Every importer goes through one
// writer that escapes exactly the characters that would otherwise read as
// mepml markup, so imported prose stays prose.
//
// Deliberately raylib-free like the rest of mepml; mep-mepml-convert-test
// round-trips test.mepml through every format.

namespace mepml {

enum class Format { Mepml, Html, Markdown, Org, Rtf, Docx, Odt, Latex, Pdf, Text, Pptx, Odp, Unknown };

// By file extension (.md/.markdown, .org, .htm/.html, .rtf, .docx, .odt,
// .tex, .pdf, .txt, .pptx, .odp, .mepml).
Format FormatFromPath(const std::string &path);
Format FormatFromName(const std::string &name);  // "md", "markdown", "org", "html", ...
std::string FormatExtension(Format f);           // "md", "org", "html", ...
bool CanExport(Format f);
bool CanImport(Format f);

// --- Export -------------------------------------------------------------
std::string ToMarkdown(const Document &doc);
std::string ToOrg(const Document &doc);
std::string ToPlainText(const Document &doc);
// `base_dir` resolves relative image paths (RTF embeds the pictures).
std::string ToRtf(const Document &doc, const std::string &base_dir);
// A presentation (`//? Type: presentation`, IsPresentation) is a Beamer
// deck, so its PDF is too; any other document is an article.
std::string ToLatex(const Document &doc, const std::string &base_dir);
// ToHtml, or for a presentation ToSlidesHtml.
std::string ToHtmlFor(const Document &doc);
// Binary packages, written to `path`; false with *error on failure.
bool WriteDocx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
bool WriteOdt(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
// The document's slides (src/mepml_slides.cpp) as PowerPoint and Impress
// decks: a title slide from the header, then one per \slide, each titled by
// its first heading. False with *error when there is no slide.
bool WritePptx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
bool WriteOdp(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
// Any exportable format except PDF, chosen by `path`'s extension.
bool ExportFile(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);

// --- Import -------------------------------------------------------------
std::string FromMarkdown(const std::string &md);
std::string FromOrg(const std::string &org);
std::string FromHtml(const std::string &html);
std::string FromRtf(const std::string &rtf);
// DOCX/ODT bytes through mep's office document model -- ImportFile's
// fallback when its own readers cannot make sense of a package. Embedded
// images are written into `media_dir` (created on demand) and referenced as
// `media_rel`/imageN.ext from the result.
bool FromOffice(const std::string &bytes, bool odt, const std::string &media_dir, const std::string &media_rel,
                std::string *mepml, std::string *error);
// Any importable file, by extension. Images from DOCX/ODT/RTF go to a
// "<stem>_media" directory beside `out_path` (the .mepml being written).
bool ImportFile(const std::string &in_path, const std::string &out_path, std::string *mepml, std::string *error);

// --- Shared by the importers (exposed for tests) -------------------------
// `text` as mepml prose: characters that would start markup are escaped
// where they would, and only there.
std::string EscapeInline(const std::string &text);
// One line of prose that must not be read as a block: a leading `>`, `//`,
// `- `, `|`, `@`, ``` ... is escaped too.
std::string EscapeLineStart(const std::string &line);

}  // namespace mepml

#endif  // MEP_MEPML_CONVERT_H
