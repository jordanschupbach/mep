#ifndef MEP_MEPML_CONVERT_H
#define MEP_MEPML_CONVERT_H

#include <functional>
#include <string>
#include <utility>
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
// What an export of `doc` to `f` is called, for \when / \raw
// (FormatsMatch), most specific first: html {"html"} ({"html", "slides"}
// for a presentation's slideshow), tex {"tex", "latex"} (a Beamer deck
// {"beamer", "tex", "latex", "slides"}), pdf the same with "pdf" first, md
// {"md", "markdown"}, docx {"docx", "word", "office"}, pptx {"pptx",
// "powerpoint", "office", "slides"} ... `beamer` is the Beamer deck
// whatever the document's Type. Mepml itself has none.
std::vector<std::string> ExportTags(Format f, const Document &doc, bool beamer = false);

// --- Export -------------------------------------------------------------
// An export is a copy: the .mepml is the file to edit. The text exports say
// so in a comment at the top -- `# ...` in Org, `<!-- ... -->` in Markdown
// (after its front matter) and HTML (after the doctype), `% ...` in LaTeX --
// unless the options turn the notice off or the document does (`//? Notice:
// no`). Plain text and RTF have no comment to put it in, and the office
// packages carry their source in their document properties already.
struct ExportOptions {
    bool notice = true;
    std::string source;  // the .mepml's name, for the notice ("README.mepml"); "" names none
};
// `text`, an export to `f`, with the notice in front where the format has a
// place for it and the options and the document allow one.
std::string WithGeneratedNotice(Format f, const Document &doc, const ExportOptions &opts, const std::string &text);
// Whether `line` is such a notice in any of those formats (comment markers
// included), and `text` without one: the importers drop it, so a document
// exported and read back is the document, not one with a comment about
// itself.
bool IsGeneratedNotice(const std::string &line);
std::string StripGeneratedNotice(const std::string &text);

std::string ToMarkdown(const Document &doc);
std::string ToOrg(const Document &doc);
std::string ToPlainText(const Document &doc);
// `base_dir` resolves relative image paths (RTF embeds the pictures).
std::string ToRtf(const Document &doc, const std::string &base_dir);
// A presentation (`//? Type: presentation`, IsPresentation) is a Beamer
// deck, so its PDF is too; any other document is an article.
std::string ToLatex(const Document &doc, const std::string &base_dir);
// The Beamer deck whatever the document's Type: a title frame from the
// header, then a frame per \slide. "" with *error when there is no slide.
std::string ToBeamer(const Document &doc, const std::string &base_dir, std::string *error);
// ToHtml, or for a presentation ToSlidesHtml. The second form is for a
// page written to a file: `path` and `base_dir` (the document's
// directory) let it name the document's pictures and file links from
// where it is written (HtmlOptions::out_dir).
std::string ToHtmlFor(const Document &doc);
std::string ToHtmlFor(const Document &doc, const std::string &path, const std::string &base_dir);
// Binary packages, written to `path`; false with *error on failure.
bool WriteDocx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
bool WriteOdt(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
// The document's slides (src/mepml_slides.cpp) as PowerPoint and Impress
// decks: a title slide from the header, then one per \slide, each titled by
// its first heading. False with *error when there is no slide.
bool WritePptx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
bool WriteOdp(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error);
// A picture of a TeX equation: PNG bytes and the size it is set at, in
// points. A .pptx's display equations carry one for the readers that
// cannot show Office Math (LibreOffice Impress is one). The editor sets a
// renderer (its own typesetter); without one -- mep-mepml -- such a reader
// gets the equation as text instead.
struct MathPicture {
    std::string png;
    double width_pt = 0, height_pt = 0;
};
using MathPictureRenderer = std::function<bool(const std::string &tex, bool display, double pt, MathPicture *out)>;
void SetMathPictureRenderer(MathPictureRenderer renderer);
// Any exportable format except PDF, chosen by `path`'s extension, with
// the generated-file notice `opts` asks for.
bool ExportFile(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error,
                const ExportOptions &opts = {});

// --- Where an export goes, and what runs after it ---------------------------
// Exports are built into a directory, `build` beside the document unless
// the header says otherwise: `//? Build: out` (relative to the document;
// `.` is the document's own directory). After an export the document's
// `//? Post:` command, if it has one, runs in the document's directory
// through the shell with MEP_SOURCE (the .mepml), MEP_BUILD (the build
// directory) and MEP_OUT (the file(s) just written, space-separated) in
// its environment -- `//? Post: cp ./build/README.org .` copies a build
// back beside the source. The pictures an export names by a relative
// path (in Org, Markdown and HTML) are written as the document names
// them, and ExportFile copies each one into the build directory at that
// same path (CopyPictures), so build/README.md finds build/assets/logo.png
// where it is written and README.md finds assets/logo.png once a Post
// command has copied it back beside the source.
std::string BuildDir(const Document &doc);     // the header's value, "build" without one
std::string PostCommand(const Document &doc);  // the header's value, "" without one
// `<document dir>/<build dir>/<stem>.<ext>` for `source` (a .mepml path;
// "" is an untitled buffer, exported under `untitled` in the working
// directory's build dir). An absolute Build: is used as it is.
std::string ExportPath(const Document &doc, const std::string &source, const std::string &ext);
// Creates `path`'s directory (and every missing parent) so the export can
// be written; false, with `*error`, when that fails.
bool EnsureExportDir(const std::string &path, std::string *error);
// The pictures `doc` names by a relative path that stays inside its own
// directory -- \image blocks, the figures a code block produced, picture
// cells -- as written, each once, in document order. URLs, absolute paths
// and paths that climb out (`../x.png`) are left out: they are not copied,
// and the HTML export names them from where it is written instead.
std::vector<std::string> DocumentPictures(const Document &doc);
// Copies each of those from base_dir (the document's directory) into
// out_dir at the same relative path, making directories as needed, so an
// export written into out_dir finds them. Nothing is done when the two
// are the same directory; a picture that does not exist is skipped (the
// export still names it); an existing copy is overwritten only when the
// source is newer. False, with `*error`, when a copy fails.
bool CopyPictures(const Document &doc, const std::string &base_dir, const std::string &out_dir, std::string *error);
// The Post command's environment for an export of `source` that wrote
// `outs`: MEP_SOURCE, MEP_BUILD, MEP_OUT (see above). Absolute paths.
std::vector<std::pair<std::string, std::string>> PostEnvironment(const Document &doc, const std::string &source,
                                                                 const std::vector<std::string> &outs);
// The Post command as a Bourne-shell script with that environment
// exported in front, for `sh -c`; "" when the document has no Post.
std::string PostShellScript(const Document &doc, const std::string &source, const std::vector<std::string> &outs);
// Runs the Post command (if any) in the document's directory, blocking;
// true when there is none or it exited 0, else false with `*error`.
bool RunPostCommand(const Document &doc, const std::string &source, const std::vector<std::string> &outs, std::string *error);

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
// Org's own block names (src, example, quote, verse, center, comment,
// export, abstract): `#+begin_example` is Org's literal example block, never
// mep's example box. A box whose kind is one of these is written
// `#+begin_box_<kind>` by the Org export and read back from it.
bool IsOrgBlockName(const std::string &name);

}  // namespace mepml

#endif  // MEP_MEPML_CONVERT_H
