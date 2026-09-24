// mep's own org-mode language server, analysis half (see org_lsp.h for
// the scope statement and position conventions; org_lsp_server.cpp is the
// JSON-RPC wire half that turns these functions into the `mep-org-lsp`
// binary).
//
// Everything here is one of three shapes:
//   1. a small matcher over a single line (MatchBlockBegin, MatchKeyword,
//      ScanTimestamps, ...),
//   2. ScanStructure, the one stateful pass that walks the document
//      keeping an open-block stack and an open-drawer marker -- both the
//      linter and the completer need to know "is this line inside a
//      #+begin_src" and neither should re-derive it, and
//   3. the public entry points, which compose 1 and 2.
//
// Nothing here parses org into an element tree. That is deliberate: the
// checks worth having are all local ("this end doesn't match that begin",
// "this date has a month 13", "this link never closes"), and a real
// parser would have to take a position on every ambiguity org resolves by
// context, turning a linter that is quiet when unsure into one that is
// confidently wrong.

#include "org_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "org_doc.h"

namespace {

// --- Tiny string helpers ----------------------------------------------

/** @brief Lowercases an ASCII string (org keyword/block names are ASCII by construction). */
std::string Lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/** @brief Uppercases an ASCII string. */
std::string Upper(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/** @brief Returns the byte offset of the first non-space/tab character, or the string length. */
size_t IndentEnd(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/** @brief Reports whether a string is entirely spaces and tabs (or empty). */
bool AllBlank(const std::string &s, size_t from = 0) {
    for (size_t i = from; i < s.size(); i++) {
        if (s[i] != ' ' && s[i] != '\t') return false;
    }
    return true;
}

/** @brief Trims leading and trailing spaces/tabs. */
std::string Trim(const std::string &s) {
    size_t b = IndentEnd(s);
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
}

/** @brief Case-insensitive equality for ASCII strings. */
bool IEq(const std::string &a, const std::string &b) { return Lower(a) == Lower(b); }

/** @brief Reports whether `s` starts with `p`, case-insensitively. */
bool IStartsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && Lower(s.substr(0, p.size())) == Lower(p);
}

/** @brief Word characters for the alnum/underscore prefix scan mep's completion client uses. */
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// Levenshtein distance, capped: only ever used to decide "is this
// unknown keyword within typo distance of a real one", so anything past
// `limit` is indistinguishable from "not close" and the early-out keeps
// a document full of custom keywords from paying for a full matrix
// against the whole vocabulary.
/** @brief Edit distance between two strings, saturating at `limit + 1`. */
int EditDistance(const std::string &a, const std::string &b, int limit) {
    const size_t n = a.size(), m = b.size();
    if (a == b) return 0;
    if (n > m + static_cast<size_t>(limit) || m > n + static_cast<size_t>(limit)) return limit + 1;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; i++) {
        cur[0] = static_cast<int>(i);
        int row_best = cur[0];
        for (size_t j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            row_best = std::min(row_best, cur[j]);
        }
        if (row_best > limit) return limit + 1;
        prev.swap(cur);
    }
    return prev[m];
}

// --- Vocabulary -------------------------------------------------------
//
// Every table below is {name, detail, doc}: `detail` is the short
// right-hand annotation a completion list shows, `doc` the sentence hover
// and the completion side panel show. Kept as plain data (and exposed
// through org_lsp.h) so the linter's "did you mean" suggestions, the
// completer's candidate list and hover all answer from one source rather
// than three lists that drift apart.

const std::vector<OrgLspVocabEntry> kKeywords = {
    {"TITLE", "document title", "The exported document's title."},
    {"SUBTITLE", "document subtitle", "A subtitle, rendered under the title by exporters that support one."},
    {"AUTHOR", "author name", "The author line of the exported document."},
    {"EMAIL", "author email", "The author's email address, exported alongside AUTHOR."},
    {"DATE", "document date", "The document's date; may be a timestamp or free text."},
    {"DESCRIPTION", "export description", "Long description, exported as HTML meta description / LaTeX abstract material."},
    {"KEYWORDS", "export keywords", "Comma-separated keywords, exported as an HTML meta keywords tag."},
    {"LANGUAGE", "export language", "Language code (e.g. `en`) selecting the exporter's own fixed strings."},
    {"OPTIONS", "export switches", "Space-separated `key:value` export switches, e.g. `toc:nil num:2`."},
    {"STARTUP", "visibility/behavior", "Space-separated words applied when the file is opened, e.g. `overview indent`."},
    {"FILETAGS", "inherited tags", "Tags inherited by every headline in the file, written `:tag1:tag2:`."},
    {"TAGS", "tag completion set", "Declares the file's tag vocabulary (and optional mutually exclusive groups)."},
    {"TODO", "workflow keywords", "The file's TODO keyword sequence, e.g. `TODO NEXT | DONE CANCELED`."},
    {"SEQ_TODO", "sequential workflow", "Like TODO, explicitly declaring a sequential workflow."},
    {"TYP_TODO", "type workflow", "Like TODO, but the keywords are types rather than a sequence."},
    {"PRIORITIES", "highest/lowest/default", "Three letters: the highest, lowest and default priority cookie."},
    {"CATEGORY", "agenda category", "Category shown for this file's entries in the agenda."},
    {"COLUMNS", "column view format", "The default column-view format string for this file."},
    {"CONSTANTS", "table constants", "`name=value` pairs usable as `$name` inside table formulas."},
    {"DRAWERS", "extra drawer names", "Additional drawer names this file uses beyond the built-in ones."},
    {"LINK", "link abbreviation", "Defines a link abbreviation, e.g. `#+LINK: gh https://github.com/%s`."},
    {"MACRO", "macro definition", "Defines a `{{{name}}}` macro expanded at export time."},
    {"NAME", "element name", "Names the element that follows, so `[[name]]` and `#+CALL:` can refer to it."},
    {"CAPTION", "element caption", "Caption for the following table, figure or block."},
    {"HEADER", "block header args", "Header arguments applied to the single block that follows."},
    {"PROPERTY", "file-wide property", "Sets a file-wide property, e.g. `#+PROPERTY: header-args:python :results output`."},
    {"RESULTS", "babel results marker", "Marks the output of the preceding named source block."},
    {"CALL", "call a named block", "Evaluates a named source block, e.g. `#+CALL: build(target=\"all\")`."},
    {"SETUPFILE", "shared settings file", "Path or URL of another org file whose keywords are read in here."},
    {"INCLUDE", "include a file", "Splices another file into the export, e.g. `#+INCLUDE: \"part.org\" :lines \"5-10\"`."},
    {"BIND", "export-time variable", "Binds an Emacs variable during export (only honored when org-export-allow-bind-keywords is on)."},
    {"ARCHIVE", "archive location", "Default target location for archived subtrees in this file."},
    {"SELECT_TAGS", "export selection", "Tags whose subtrees are exported when any of them is present."},
    {"EXCLUDE_TAGS", "export exclusion", "Tags whose subtrees are never exported."},
    {"CREATOR", "creator line", "Overrides the \"created with\" line exporters emit."},
    {"TBLFM", "table formula", "Column/field formulas for the table immediately above."},
    {"PLOT", "table plot options", "Plotting parameters for the table immediately above."},
    {"TOC", "inline table of contents", "Inserts a table of contents at this point, e.g. `#+TOC: headlines 2`."},
    {"BIBLIOGRAPHY", "citation sources", "Bibliography file(s) for org-cite."},
    {"CITE_EXPORT", "citation style", "Citation processor and style for org-cite."},
    {"PRINT_BIBLIOGRAPHY", "render bibliography", "Renders the bibliography at this point."},
    {"MATHJAX", "MathJax options", "MathJax configuration for the HTML exporter."},
    {"INFOJS_OPT", "org-info.js options", "Options for the HTML exporter's org-info.js viewer."},
    {"LATEX_HEADER", "LaTeX preamble line", "A line added to the LaTeX preamble."},
    {"LATEX_HEADER_EXTRA", "extra LaTeX preamble", "Like LATEX_HEADER, but excluded from the `beamer`/`latex` class defaults."},
    {"LATEX_CLASS", "LaTeX document class", "Selects a class from org-latex-classes, e.g. `article`."},
    {"LATEX_CLASS_OPTIONS", "class options", "Options appended to `\\documentclass`, e.g. `[11pt,a4paper]`."},
    {"LATEX_COMPILER", "pdflatex/xelatex/lualatex", "Which LaTeX compiler the exporter should invoke."},
    {"HTML_HEAD", "HTML <head> line", "A line inserted into the exported HTML `<head>`."},
    {"HTML_HEAD_EXTRA", "extra HTML <head>", "Like HTML_HEAD, appended after it."},
    {"HTML_DOCTYPE", "HTML doctype", "The doctype the HTML exporter emits, e.g. `html5`."},
    {"HTML_CONTAINER", "HTML section element", "The element wrapping each exported section, e.g. `div`."},
    {"HTML_LINK_UP", "\"up\" link", "URL for the exported page's \"up\" navigation link."},
    {"HTML_LINK_HOME", "\"home\" link", "URL for the exported page's \"home\" navigation link."},
    {"BEAMER_HEADER", "Beamer preamble line", "A line added to the Beamer preamble."},
    {"BEAMER_THEME", "Beamer theme", "The Beamer outer theme, e.g. `Madrid`."},
    {"BEAMER_FONT_THEME", "Beamer font theme", "The Beamer font theme."},
    {"BEAMER_COLOR_THEME", "Beamer color theme", "The Beamer color theme."},
    {"BEAMER_INNER_THEME", "Beamer inner theme", "The Beamer inner theme."},
    {"BEAMER_OUTER_THEME", "Beamer outer theme", "The Beamer outer theme."},
    {"ODT_STYLES_FILE", "ODT styles", "Styles file used by the ODT exporter."},
    {"TEXINFO_CLASS", "Texinfo class", "Selects a class from org-texinfo-classes."},
    {"TEXINFO_FILENAME", "Texinfo output name", "Output file name for the Texinfo exporter."},
    {"TEXINFO_HEADER", "Texinfo preamble line", "A line added to the Texinfo preamble."},
    // Raw backend lines: everything after the colon is passed straight
    // through to that one exporter and ignored by every other.
    {"LATEX", "raw LaTeX", "A line of raw LaTeX, emitted only by the LaTeX exporter."},
    {"HTML", "raw HTML", "A line of raw HTML, emitted only by the HTML exporter."},
    {"ASCII", "raw ASCII", "A line of raw text, emitted only by the ASCII exporter."},
    {"BEAMER", "raw Beamer", "A line of raw LaTeX, emitted only by the Beamer exporter."},
    {"TEXINFO", "raw Texinfo", "A line of raw Texinfo, emitted only by the Texinfo exporter."},
    {"ODT", "raw ODT", "A line of raw ODT XML, emitted only by the ODT exporter."},
    {"MAN", "raw man", "A line of raw roff, emitted only by the man exporter."},
    {"MD", "raw Markdown", "A line of raw Markdown, emitted only by the Markdown exporter."},
    // Dynamic blocks: `#+BEGIN: clocktable :maxlevel 2` ... `#+END:`,
    // whose body is regenerated by a named writer function. Note the
    // colon directly after BEGIN/END -- unlike `#+begin_src` these are
    // keyword lines, which is why they live in this table.
    {"BEGIN", "dynamic block", "Opens a dynamic block: `#+BEGIN: name :args` ... `#+END:`."},
    {"END", "dynamic block end", "Closes the dynamic block opened by the nearest `#+BEGIN:`."},
};

// Keyword *prefixes* that are open-ended by design: any `#+ATTR_<backend>:`
// or `#+<BACKEND>_...:` is legitimate, so an unknown-keyword warning must
// not fire on one. Checked before the did-you-mean pass.
const char *const kKeywordPrefixes[] = {"ATTR_",  "EXPORT_", "LATEX_", "HTML_",  "BEAMER_", "TEXINFO_",
                                        "ODT_",   "ASCII_",  "MD_",    "MAN_",   "KINDLE_", "EPUB_",
                                        "KOMA_",  "ORG_",    "RSS_",   "HUGO_",  "JEKYLL_", "DOCBOOK_"};

const std::vector<OrgLspVocabEntry> kBlocks = {
    {"src", "source code", "A source block: `#+begin_src LANG :args` ... `#+end_src`. Babel evaluates it."},
    {"example", "literal example", "Literal text, exported verbatim and never parsed as org markup."},
    {"quote", "block quote", "A quotation, exported as a block quote."},
    {"verse", "poetry/verse", "Line breaks and leading whitespace are preserved."},
    {"center", "centered text", "Contents are centered by exporters that support it."},
    {"comment", "not exported", "Contents are dropped entirely at export time."},
    {"export", "raw backend text", "Raw text passed straight to one backend: `#+begin_export html`."},
    {"verbatim", "verbatim text", "Literal text, like `example`, kept for compatibility."},
    {"abstract", "abstract", "A special block exporters may render as the document abstract."},
    {"proof", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"theorem", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"definition", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"remark", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"note", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"warning", "special block", "A special block; exporters wrap it in an environment of the same name."},
    {"aside", "special block", "A special block; exporters wrap it in an environment of the same name."},
};

// Blocks whose contents org never parses as org markup. Everything the
// linter does inside a document (links, timestamps, footnotes, nested
// blocks, drawers) is suppressed within these.
bool IsLiteralBlock(const std::string &lower_name) {
    return lower_name == "src" || lower_name == "example" || lower_name == "export" || lower_name == "comment" ||
           lower_name == "verbatim";
}

const std::vector<OrgLspVocabEntry> kHeaderArgs = {
    {"results", "value|output + format", "How to collect and format the block's output, e.g. `:results output verbatim`."},
    {"exports", "code|results|both|none", "What export includes for this block."},
    {"session", "named session", "Run in a persistent named session rather than a one-shot process."},
    {"var", "name=value", "Binds a variable inside the block; repeatable."},
    {"file", "output path", "Write the result to this file and link to it."},
    {"file-desc", "link description", "Description used for the `:file` link."},
    {"output-dir", "directory", "Directory the `:file` result is written into."},
    {"dir", "working directory", "Directory the block runs in."},
    {"tangle", "no|yes|path", "Whether (and where) `org-babel-tangle` writes this block."},
    {"mkdirp", "yes|no", "Create missing parent directories when tangling."},
    {"comments", "no|link|yes|org|both|noweb", "Comment style inserted around tangled code."},
    {"padline", "yes|no", "Insert a blank line around tangled blocks."},
    {"shebang", "#!/bin/sh", "Shebang line prepended to the tangled file (which is then made executable)."},
    {"noweb", "no|yes|tangle|no-export|strip-export|eval", "When `<<name>>` noweb references are expanded."},
    {"noweb-ref", "reference name", "Name this block answers to when referenced by noweb syntax."},
    {"noweb-sep", "separator", "String joining multiple blocks sharing one noweb-ref."},
    {"cache", "no|yes", "Skip re-evaluation while the block and its inputs are unchanged."},
    {"eval", "never|query|no-export|never-export|query-export", "Restricts when the block may be evaluated."},
    {"hlines", "no|yes", "Whether horizontal table rules are passed through to the block."},
    {"colnames", "no|yes|nil", "Whether the first table row is treated as column names."},
    {"rownames", "no|yes", "Whether the first table column is treated as row names."},
    {"sep", "field separator", "Field separator used when reading a table result."},
    {"wrap", "block name", "Wraps the result in `#+begin_<name>` ... `#+end_<name>`."},
    {"post", "post-processing", "A babel call applied to this block's result before insertion."},
    {"prologue", "code prefix", "Raw code prepended to the block body before evaluation."},
    {"epilogue", "code suffix", "Raw code appended to the block body before evaluation."},
    {"main", "yes|no", "mep extension: whether to wrap the body in a generated `main` for compiled languages."},
    {"flags", "compiler flags", "Extra flags passed to the compiler for a compiled-language block."},
    {"includes", "#include lines", "Extra include directives for a C/C++ block."},
    {"cmdline", "program arguments", "Arguments appended to the program invocation."},
    {"stdin", "stdin text", "Text fed to the block's standard input."},
    {"screen-width", "columns", "Width GAP displays values at before wrapping them (mep's `gap` backend)."},
    {"memory", "workspace limit", "Size a `gap` block's workspace will not grow past, e.g. `2g` (mep's `gap` backend)."},
    {"packages", "yes|no", "Whether a `gap` block autoloads GAP's packages (mep's `gap` backend)."},
};

// Enumerated header-argument values, for both completion and the
// "unknown value" warning. A key absent here takes free-form values and
// is never value-checked.
const std::map<std::string, std::vector<const char *>> kHeaderArgValues = {
    {"results", {"value", "output", "table", "vector", "list", "scalar", "verbatim", "file", "link", "graphics", "raw",
                 "html", "latex", "code", "pp", "drawer", "org", "replace", "silent", "none", "append", "prepend"}},
    {"exports", {"code", "results", "both", "none"}},
    {"cache", {"no", "yes"}},
    {"hlines", {"no", "yes"}},
    {"colnames", {"no", "yes", "nil"}},
    {"rownames", {"no", "yes"}},
    {"padline", {"yes", "no"}},
    {"mkdirp", {"yes", "no"}},
    {"main", {"yes", "no"}},
    {"packages", {"yes", "no"}},
    {"noweb", {"no", "yes", "tangle", "no-export", "strip-export", "eval"}},
    {"eval", {"never", "query", "no-export", "never-export", "query-export", "yes", "no"}},
    {"comments", {"no", "link", "yes", "org", "both", "noweb"}},
};

const std::vector<OrgLspVocabEntry> kOptions = {
    {"toc:", "nil | N | t", "Table of contents: `nil` for none, a number for a depth limit."},
    {"num:", "nil | N | t", "Section numbering, optionally limited to a depth."},
    {"H:", "N", "Headline levels exported as sections rather than list items."},
    {"author:", "t | nil", "Include the author line."},
    {"email:", "t | nil", "Include the email address."},
    {"date:", "t | nil", "Include the date line."},
    {"creator:", "t | nil", "Include the \"created with\" line."},
    {"timestamp:", "t | nil", "Include the export timestamp."},
    {"title:", "t | nil", "Include the title."},
    {"todo:", "t | nil", "Export TODO keywords."},
    {"tags:", "t | nil | not-in-toc", "Export headline tags."},
    {"pri:", "t | nil", "Export priority cookies."},
    {"tasks:", "t | nil | todo | done | LIST", "Which TODO entries to export."},
    {"stat:", "t | nil", "Export `[1/3]` statistics cookies."},
    {"prop:", "t | nil | LIST", "Export property drawers."},
    {"d:", "t | nil | LIST", "Export drawers."},
    {"f:", "t | nil", "Export footnotes."},
    {"*:", "t | nil", "Honor `*bold*` emphasis markers."},
    {"/:", "t | nil", "Honor `/italic/` emphasis markers."},
    {"-:", "t | nil", "Convert `--` and `---` to en/em dashes."},
    {"::", "t | nil", "Honor fixed-width `:` lines."},
    {"<:", "t | nil", "Export timestamps."},
    {"\\n:", "t | nil", "Preserve line breaks."},
    {"^:", "t | nil | {}", "Interpret `a^b` as superscript (`{}` requires braces)."},
    {"_:", "t | nil | {}", "Interpret `a_b` as subscript (`{}` requires braces)."},
    {"|:", "t | nil", "Export tables."},
    {"e:", "t | nil", "Export org entities such as `\\alpha`."},
    {"tex:", "t | nil | verbatim", "How LaTeX fragments are exported."},
    {"p:", "t | nil", "Export planning (SCHEDULED/DEADLINE/CLOSED) lines."},
    {"inline:", "t | nil", "Export inline tasks."},
    {"arch:", "headline | t | nil", "How archived subtrees are exported."},
    {"c:", "t | nil", "Export clock entries."},
    {"broken-links:", "t | nil | mark", "What to do with links that do not resolve."},
};

const std::vector<OrgLspVocabEntry> kStartup = {
    {"overview", "visibility", "Open the file folded to top-level headlines."},
    {"content", "visibility", "Open the file showing all headlines, no body text."},
    {"showall", "visibility", "Open the file fully unfolded."},
    {"showeverything", "visibility", "Open fully unfolded, drawers included."},
    {"show2levels", "visibility", "Open showing two headline levels."},
    {"show3levels", "visibility", "Open showing three headline levels."},
    {"show4levels", "visibility", "Open showing four headline levels."},
    {"show5levels", "visibility", "Open showing five headline levels."},
    {"indent", "indentation", "Turn on virtual indentation (org-indent-mode)."},
    {"noindent", "indentation", "Turn off virtual indentation."},
    {"hidestars", "indentation", "Hide all but the last headline star."},
    {"showstars", "indentation", "Show every headline star."},
    {"odd", "headline levels", "Use only odd headline levels (`*`, `***`, ...)."},
    {"oddeven", "headline levels", "Use consecutive headline levels."},
    {"align", "tables", "Align all tables when the file is opened."},
    {"noalign", "tables", "Do not align tables on open."},
    {"inlineimages", "images", "Display inline images when the file is opened."},
    {"noinlineimages", "images", "Do not display inline images on open."},
    {"logdone", "logging", "Record a timestamp when an entry is marked DONE."},
    {"lognotedone", "logging", "Record a timestamped note when an entry is marked DONE."},
    {"nologdone", "logging", "Do not log DONE transitions."},
    {"logrepeat", "logging", "Record a note when a repeating task is reset."},
    {"nologrepeat", "logging", "Do not log repeat resets."},
    {"logreschedule", "logging", "Log rescheduling of an entry."},
    {"logredeadline", "logging", "Log deadline changes."},
    {"logdrawer", "logging", "Store log notes in the LOGBOOK drawer."},
    {"nologdrawer", "logging", "Store log notes directly under the headline."},
    {"logstatesreversed", "logging", "Add new log entries at the end of the drawer."},
    {"hideblocks", "visibility", "Fold `#+begin_...` blocks when the file is opened."},
    {"nohideblocks", "visibility", "Do not fold blocks on open."},
    {"hidedrawers", "visibility", "Fold drawers when the file is opened."},
    {"entitiespretty", "display", "Display `\\alpha` and friends as their UTF-8 characters."},
    {"fninline", "footnotes", "Define footnotes inline, at the reference."},
    {"fnlocal", "footnotes", "Define footnotes in the current outline section."},
    {"fnauto", "footnotes", "Automatically number new footnotes `[fn:N]`."},
    {"fnprompt", "footnotes", "Prompt for a footnote label."},
    {"fnadjust", "footnotes", "Renumber footnotes automatically."},
    {"constcgs", "tables", "Use CGS units for table constants."},
    {"constSI", "tables", "Use SI units for table constants."},
};

const std::vector<OrgLspVocabEntry> kProperties = {
    {"ID", "globally unique id", "A unique id other files link to with `[[id:...]]`."},
    {"CUSTOM_ID", "document-local id", "A human-readable id linked to with `[[#custom-id]]`."},
    {"CATEGORY", "agenda category", "Agenda category for this subtree."},
    {"EFFORT", "H:MM", "Estimated effort, used by the agenda's clock report and mep's Gantt view."},
    {"ARCHIVE", "archive location", "Where this subtree is archived to."},
    {"COLUMNS", "column format", "Column-view format for this subtree."},
    {"ORDERED", "t", "Child tasks must be completed in order."},
    {"NOBLOCKING", "t", "This entry never blocks its parent from being marked DONE."},
    {"TRIGGER", "state trigger", "org-depend action taken when this entry changes state."},
    {"BLOCKER", "predecessor ids", "Entries that must be DONE first; mep's Gantt view draws these as arrows."},
    {"LOGGING", "logging overrides", "Per-subtree overrides of the file's logging settings."},
    {"EXPORT_FILE_NAME", "output path", "Output file name when exporting this subtree."},
    {"EXPORT_TITLE", "subtree title", "Title used when exporting this subtree on its own."},
    {"EXPORT_OPTIONS", "subtree options", "Export options applied when exporting this subtree."},
    {"VISIBILITY", "folded|children|content|all", "How this subtree is folded when the file is opened."},
    {"header-args", "babel defaults", "Default header arguments for source blocks in this subtree."},
    {"ASSIGNEE", "person", "mep extension: the person a task is assigned to, shown on Kanban cards."},
    {"TEAM", "team", "mep extension: the team a task belongs to, shown on Kanban cards."},
    {"PROGRESS", "0-100", "mep extension: percentage complete, drawn on the Gantt bar."},
};

// The babel languages mep itself can execute -- mep.org_babel_langs'
// exact key set (kBuiltinOrgBabel, main.cpp), including its aliases. A
// `#+begin_src` naming anything else is legal org, so the linter only
// reports it at Information severity.
const std::vector<OrgLspVocabEntry> kBabelLangs = {
    {"bash", "shell", "Runs with bash."},        {"c", "compiled", "Compiled and run with a C compiler."},
    {"clojure", "JVM", "Runs with clojure."},    {"cpp", "compiled", "Compiled and run with a C++ compiler."},
    {"crystal", "compiled", "Runs with crystal."}, {"cs", "alias of csharp", "Runs with dotnet."},
    {"csharp", "compiled", "Runs with dotnet."}, {"d", "compiled", "Runs with dmd/ldc."},
    {"elixir", "BEAM", "Runs with elixir."},     {"fortran", "compiled", "Compiled and run with gfortran."},
    {"gap", "computer algebra", "Runs with gap."},
    {"go", "compiled", "Runs with go."},         {"haskell", "compiled", "Runs with runghc/ghc."},
    {"java", "compiled", "Compiled and run with javac/java."},
    {"javascript", "Node", "Runs with node."},   {"js", "alias of javascript", "Runs with node."},
    {"julia", "Julia", "Runs with julia."},      {"kotlin", "JVM", "Runs with kotlinc."},
    {"lua", "Lua", "Runs with lua."},
    {"maxima", "computer algebra", "Runs with maxima."},
    {"nim", "compiled", "Runs with nim."},
    {"ocaml", "OCaml", "Runs with ocaml."},      {"perl", "Perl", "Runs with perl."},
    {"php", "PHP", "Runs with php."},            {"python", "Python", "Runs with python3."},
    {"r", "R", "Runs with Rscript."},            {"ruby", "Ruby", "Runs with ruby."},
    {"rust", "compiled", "Compiled and run with rustc."},
    {"scala", "JVM", "Runs with scala."},        {"sh", "shell", "Runs with bash/sh."},
    {"ts", "alias of typescript", "Runs with a TypeScript runner."},
    {"typescript", "TypeScript", "Runs with a TypeScript runner."},
    {"zig", "compiled", "Runs with zig."},
};

// Other languages org-babel commonly sees that mep cannot execute. Listed
// so the "unknown language" check stays quiet on them -- being unable to
// *run* a block is not a reason to call its language a typo.
const char *const kOtherKnownLangs[] = {"emacs-lisp", "elisp",     "lisp",   "scheme",  "dot",    "ditaa",
                                        "plantuml",   "gnuplot",   "latex",  "org",     "sql",    "sqlite",
                                        "awk",        "sed",       "make",   "octave",  "matlab",
                                        "asymptote",  "css",       "html",   "json",    "yaml",   "toml",
                                        "xml",        "text",      "conf",   "ini",     "diff",   "vim",
                                        "abc",        "calc",      "groovy", "screen",  "shell",  "eshell",
                                        "fsharp",     "forth",     "io",     "mscgen",  "picolisp", "processing",
                                        "sass",       "scss",      "stan",   "vala",    "hledger", "ledger"};

const std::vector<OrgLspVocabEntry> kEntities = {
    {"alpha", "α", "Greek small letter alpha."},      {"beta", "β", "Greek small letter beta."},
    {"gamma", "γ", "Greek small letter gamma."},      {"delta", "δ", "Greek small letter delta."},
    {"epsilon", "ε", "Greek small letter epsilon."},  {"zeta", "ζ", "Greek small letter zeta."},
    {"eta", "η", "Greek small letter eta."},          {"theta", "θ", "Greek small letter theta."},
    {"iota", "ι", "Greek small letter iota."},        {"kappa", "κ", "Greek small letter kappa."},
    {"lambda", "λ", "Greek small letter lambda."},    {"mu", "μ", "Greek small letter mu."},
    {"nu", "ν", "Greek small letter nu."},            {"xi", "ξ", "Greek small letter xi."},
    {"pi", "π", "Greek small letter pi."},            {"rho", "ρ", "Greek small letter rho."},
    {"sigma", "σ", "Greek small letter sigma."},      {"tau", "τ", "Greek small letter tau."},
    {"upsilon", "υ", "Greek small letter upsilon."},  {"phi", "φ", "Greek small letter phi."},
    {"chi", "χ", "Greek small letter chi."},          {"psi", "ψ", "Greek small letter psi."},
    {"omega", "ω", "Greek small letter omega."},      {"Alpha", "Α", "Greek capital letter alpha."},
    {"Beta", "Β", "Greek capital letter beta."},      {"Gamma", "Γ", "Greek capital letter gamma."},
    {"Delta", "Δ", "Greek capital letter delta."},    {"Theta", "Θ", "Greek capital letter theta."},
    {"Lambda", "Λ", "Greek capital letter lambda."},  {"Xi", "Ξ", "Greek capital letter xi."},
    {"Pi", "Π", "Greek capital letter pi."},          {"Sigma", "Σ", "Greek capital letter sigma."},
    {"Phi", "Φ", "Greek capital letter phi."},        {"Psi", "Ψ", "Greek capital letter psi."},
    {"Omega", "Ω", "Greek capital letter omega."},    {"rarr", "→", "Rightwards arrow."},
    {"larr", "←", "Leftwards arrow."},                {"uarr", "↑", "Upwards arrow."},
    {"darr", "↓", "Downwards arrow."},                {"harr", "↔", "Left right arrow."},
    {"rArr", "⇒", "Rightwards double arrow."},        {"lArr", "⇐", "Leftwards double arrow."},
    {"hArr", "⇔", "Left right double arrow."},        {"to", "→", "Rightwards arrow."},
    {"times", "×", "Multiplication sign."},           {"div", "÷", "Division sign."},
    {"plusmn", "±", "Plus-minus sign."},              {"le", "≤", "Less-than or equal to."},
    {"ge", "≥", "Greater-than or equal to."},         {"ne", "≠", "Not equal to."},
    {"approx", "≈", "Almost equal to."},              {"equiv", "≡", "Identical to."},
    {"propto", "∝", "Proportional to."},              {"infin", "∞", "Infinity."},
    {"partial", "∂", "Partial differential."},        {"nabla", "∇", "Nabla."},
    {"int", "∫", "Integral."},                        {"sum", "∑", "N-ary summation."},
    {"prod", "∏", "N-ary product."},                  {"radic", "√", "Square root."},
    {"forall", "∀", "For all."},                      {"exist", "∃", "There exists."},
    {"isin", "∈", "Element of."},                     {"notin", "∉", "Not an element of."},
    {"sub", "⊂", "Subset of."},                       {"sup", "⊃", "Superset of."},
    {"cap", "∩", "Intersection."},                    {"cup", "∪", "Union."},
    {"empty", "∅", "Empty set."},                     {"deg", "°", "Degree sign."},
    {"copy", "©", "Copyright sign."},                 {"reg", "®", "Registered sign."},
    {"trade", "™", "Trade mark sign."},               {"hellip", "…", "Horizontal ellipsis."},
    {"mdash", "—", "Em dash."},                       {"ndash", "–", "En dash."},
    {"nbsp", " ", "No-break space."},                 {"laquo", "«", "Left double angle quote."},
    {"raquo", "»", "Right double angle quote."},      {"ldquo", "“", "Left double quote."},
    {"rdquo", "”", "Right double quote."},            {"lsquo", "‘", "Left single quote."},
    {"rsquo", "’", "Right single quote."},            {"dagger", "†", "Dagger."},
    {"Dagger", "‡", "Double dagger."},                {"bull", "•", "Bullet."},
    {"middot", "·", "Middle dot."},                   {"euro", "€", "Euro sign."},
    {"pound", "£", "Pound sign."},                    {"yen", "¥", "Yen sign."},
    {"sect", "§", "Section sign."},                   {"para", "¶", "Pilcrow sign."},
    {"checkmark", "✓", "Check mark."},                {"star", "⋆", "Star operator."},
    {"space", " ", "A space (useful before a `\\` line break)."},
    {"under", "_", "A literal underscore."},          {"ast", "*", "A literal asterisk."},
    {"slash", "/", "A literal slash."},               {"tilde", "~", "A literal tilde."},
};

}  // namespace

const std::vector<OrgLspVocabEntry> &OrgLspKeywordVocab() { return kKeywords; }
const std::vector<OrgLspVocabEntry> &OrgLspBlockVocab() { return kBlocks; }
const std::vector<OrgLspVocabEntry> &OrgLspHeaderArgVocab() { return kHeaderArgs; }
const std::vector<OrgLspVocabEntry> &OrgLspOptionVocab() { return kOptions; }
const std::vector<OrgLspVocabEntry> &OrgLspStartupVocab() { return kStartup; }
const std::vector<OrgLspVocabEntry> &OrgLspPropertyVocab() { return kProperties; }
const std::vector<OrgLspVocabEntry> &OrgLspBabelLangVocab() { return kBabelLangs; }
const std::vector<OrgLspVocabEntry> &OrgLspEntityVocab() { return kEntities; }

namespace {

// --- Line matchers ----------------------------------------------------

// A `#+KEY:` / `#+KEY[attr]:` keyword line. `has_prefix` is true for any
// line whose first non-blank characters are `#+`, even a malformed one --
// that is what lets the linter say "this keyword is missing its colon"
// instead of silently treating the line as a paragraph the way org does.
struct KeywordMatch {
    bool has_prefix = false;
    bool ok = false;  // a well-formed "#+KEY:" (or "#+KEY[attr]:")
    std::string key;  // exactly as written, no attr, no colon
    std::string attr;  // the "[...]" affix's contents, e.g. CAPTION[short]
    size_t key_start = 0, key_end = 0;
    size_t value_start = 0;  // first byte after the colon (whitespace included)
    std::string value;       // trimmed
};

/** @brief Matches a `#+KEYWORD:` line, reporting the key, its optional `[attr]` affix and the value. */
KeywordMatch MatchKeyword(const std::string &line) {
    KeywordMatch m;
    size_t i = IndentEnd(line);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return m;
    m.has_prefix = true;
    m.key_start = i + 2;
    size_t j = m.key_start;
    while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) != 0 || line[j] == '_' || line[j] == '-')) j++;
    m.key_end = j;
    if (j == m.key_start) return m;  // "#+" followed by punctuation: not a keyword at all
    m.key = line.substr(m.key_start, j - m.key_start);
    if (j < line.size() && line[j] == '[') {
        size_t close = line.find(']', j);
        if (close == std::string::npos) return m;  // unterminated affix: not well-formed
        m.attr = line.substr(j + 1, close - j - 1);
        j = close + 1;
    }
    if (j >= line.size() || line[j] != ':') return m;  // has_prefix, but no colon
    m.ok = true;
    m.value_start = j + 1;
    m.value = Trim(line.substr(m.value_start));
    return m;
}

// A `#+begin_NAME args` / `#+end_NAME` line.
struct BlockMatch {
    bool ok = false;
    bool is_end = false;
    std::string name;  // as written, after `begin_`/`end_`
    size_t hash_start = 0;
    size_t name_start = 0, name_end = 0;
    std::string args;        // everything after the name, trimmed
    size_t args_start = 0;   // byte offset of `args` in the line (== name_end when args is empty)
};

/** @brief Matches a `#+begin_NAME`/`#+end_NAME` line, reporting which it is, the block name and any trailing arguments. */
BlockMatch MatchBlock(const std::string &line) {
    BlockMatch m;
    size_t i = IndentEnd(line);
    if (i + 1 >= line.size() || line[i] != '#' || line[i + 1] != '+') return m;
    m.hash_start = i;
    size_t after = i + 2;
    std::string rest = line.substr(after);
    size_t skip = 0;
    if (IStartsWith(rest, "begin_")) {
        skip = 6;
    } else if (IStartsWith(rest, "end_")) {
        skip = 4;
        m.is_end = true;
    } else {
        return m;
    }
    m.name_start = after + skip;
    size_t j = m.name_start;
    while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) != 0 || line[j] == '_' || line[j] == '-')) j++;
    if (j == m.name_start) return m;  // "#+begin_" with no name
    m.name_end = j;
    m.name = line.substr(m.name_start, j - m.name_start);
    m.ok = true;
    size_t a = j;
    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) a++;
    m.args_start = a;
    m.args = Trim(line.substr(a));
    return m;
}

/** @brief Matches a standalone drawer marker line (`:PROPERTIES:`, `:LOGBOOK:`, `:END:`), reporting its name and span. */
bool MatchDrawer(const std::string &line, std::string *name, size_t *start, size_t *end) {
    size_t i = IndentEnd(line);
    if (i >= line.size() || line[i] != ':') return false;
    size_t j = i + 1;
    while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) != 0 || line[j] == '_' || line[j] == '-' ||
                               line[j] == '@' || line[j] == '#' || line[j] == '%')) j++;
    if (j == i + 1 || j >= line.size() || line[j] != ':') return false;
    if (!AllBlank(line, j + 1)) return false;
    if (name) *name = line.substr(i + 1, j - i - 1);
    if (start) *start = i;
    if (end) *end = j + 1;
    return true;
}

/** @brief Matches a `SCHEDULED:`/`DEADLINE:`/`CLOSED:` planning line. */
bool IsPlanningLine(const std::string &line) {
    std::string t = Trim(line);
    return IStartsWith(t, "SCHEDULED:") || IStartsWith(t, "DEADLINE:") || IStartsWith(t, "CLOSED:");
}

/** @brief Matches a `#` comment line (`#` followed by a space or end of line, never `#+`). */
bool IsCommentLine(const std::string &line) {
    size_t i = IndentEnd(line);
    if (i >= line.size() || line[i] != '#') return false;
    return i + 1 >= line.size() || line[i + 1] == ' ' || line[i + 1] == '\t';
}

// --- Structural scan --------------------------------------------------

// What the linter and the completer both need to know about one line
// without re-walking the document: which block/drawer encloses it, and
// which headline it belongs to.
struct LineState {
    bool in_block = false;     // this line is *content* of some block
    bool in_literal = false;   // ... and org does not parse that content as org
    bool is_block_begin = false;
    bool is_block_end = false;
    std::string block_name;    // lowercased; the enclosing block, or the one this line opens/closes
    bool in_drawer = false;    // this line is *content* of a drawer
    bool is_drawer_begin = false;
    bool is_drawer_end = false;
    std::string drawer_name;   // uppercased
    int headline_line = -1;    // nearest preceding headline, -1 before the first one
};

struct BlockFrame {
    std::string name;  // lowercased
    int line = 0;
    bool literal = false;
    size_t hash_start = 0;  // the `#` of this line's `#+begin_`
    size_t name_end = 0;
    // A dynamic block (`#+BEGIN: clocktable` ... `#+END:`) rather than a
    // `#+begin_NAME` one. Closed by a bare `#+END:` whatever its writer
    // name, so the name-matching the ordinary case does cannot apply.
    bool dynamic = false;
};

struct StructureResult {
    std::vector<LineState> states;
    std::vector<OrgLspDiagnostic> diags;
};

/** @brief Builds a diagnostic covering `[col_start, col_end)` on `line`. */
OrgLspDiagnostic Diag(int line, size_t col_start, size_t col_end, OrgLspSeverity sev, const char *code, std::string message) {
    OrgLspDiagnostic d;
    d.line = line;
    d.col_start = static_cast<int>(col_start);
    d.col_end = static_cast<int>(col_end);
    d.severity = sev;
    d.code = code;
    d.message = std::move(message);
    return d;
}

/** @brief Builds a diagnostic covering a whole line (its indent through its last non-blank character). */
OrgLspDiagnostic DiagLine(int line, const std::string &text, OrgLspSeverity sev, const char *code, std::string message) {
    size_t b = IndentEnd(text);
    size_t e = text.size();
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) e--;
    if (e == b) e = b + 1;
    return Diag(line, b, e, sev, code, std::move(message));
}

// The one stateful pass: an open-block stack plus an open-drawer marker,
// walked top to bottom. Produces a LineState for every line and the
// structural diagnostics (mismatched/orphan/unclosed block and drawer
// markers, malformed property drawers) that only this walk can see.
/** @brief Walks the document once, producing per-line block/drawer/headline state and every structural diagnostic. */
StructureResult ScanStructure(const std::vector<std::string> &lines) {
    StructureResult out;
    out.states.resize(lines.size());
    std::vector<BlockFrame> stack;
    int cur_headline = -1;
    int prev_significant = -1;  // last line that was a headline or a planning line
    bool drawer_open = false;
    std::string drawer_name;
    int drawer_line = 0;
    size_t drawer_col_start = 0, drawer_col_end = 0;
    bool drawer_is_properties = false;

    for (size_t idx = 0; idx < lines.size(); idx++) {
        const std::string &line = lines[idx];
        const int i = static_cast<int>(idx);
        LineState &st = out.states[idx];
        st.headline_line = cur_headline;

        // Inside a literal block nothing but its own `#+end_` counts.
        if (!stack.empty() && stack.back().literal) {
            BlockMatch bm = MatchBlock(line);
            if (bm.ok && bm.is_end && IEq(bm.name, stack.back().name)) {
                st.is_block_end = true;
                st.block_name = Lower(bm.name);
                stack.pop_back();
            } else {
                st.in_block = true;
                st.in_literal = true;
                st.block_name = stack.back().name;
            }
            continue;
        }

        if (!stack.empty()) {
            st.in_block = true;
            st.block_name = stack.back().name;
        }

        BlockMatch bm = MatchBlock(line);
        if (bm.ok && !bm.is_end) {
            st.is_block_begin = true;
            st.in_block = false;  // the marker line is structure, not content
            st.block_name = Lower(bm.name);
            stack.push_back({Lower(bm.name), i, IsLiteralBlock(Lower(bm.name)), bm.hash_start, bm.name_end});
            if (drawer_open) {
                out.diags.push_back(Diag(drawer_line, drawer_col_start, drawer_col_end, OrgLspSeverity::Error, "unclosed-drawer",
                                         "Drawer :" + drawer_name + ": is never closed with `:END:`."));
                drawer_open = false;
            }
            continue;
        }
        if (bm.ok && bm.is_end) {
            st.is_block_end = true;
            st.in_block = false;
            st.block_name = Lower(bm.name);
            const std::string want = Lower(bm.name);
            size_t found = stack.size();
            for (size_t k = stack.size(); k-- > 0;) {
                if (stack[k].name == want) {
                    found = k;
                    break;
                }
            }
            // Recovery, in the order that produces one diagnostic per
            // mistake rather than two:
            //   - nothing open at all: this closer is simply orphaned;
            //   - a closer naming no open block: almost always a typo in
            //     the name, so it closes the innermost block anyway and
            //     the report names the closer that was expected (letting
            //     it stand would also report that block as unclosed);
            //   - a closer naming an *outer* block: the inner ones were
            //     left open, so they are unwound to it.
            if (stack.empty()) {
                out.diags.push_back(Diag(i, bm.hash_start, bm.name_end, OrgLspSeverity::Error, "unmatched-end",
                                         "`#+end_" + bm.name + "` closes no open block."));
            } else if (found == stack.size()) {
                out.diags.push_back(Diag(i, bm.hash_start, bm.name_end, OrgLspSeverity::Error, "mismatched-end",
                                         "Expected `#+end_" + stack.back().name + "` (opened on line " +
                                             std::to_string(stack.back().line + 1) + "), found `#+end_" + bm.name + "`."));
                stack.pop_back();
            } else {
                if (found + 1 != stack.size()) {
                    out.diags.push_back(Diag(i, bm.hash_start, bm.name_end, OrgLspSeverity::Error, "mismatched-end",
                                             "Expected `#+end_" + stack.back().name + "` (opened on line " +
                                                 std::to_string(stack.back().line + 1) + "), found `#+end_" + bm.name + "`."));
                }
                stack.resize(found);
            }
            continue;
        }

        // Dynamic blocks. `#+BEGIN:`/`#+END:` are keyword lines (note the
        // colon straight after the name), so MatchBlock never sees them,
        // but an unclosed one is the same class of error as an unclosed
        // `#+begin_src` and belongs on the same stack.
        const KeywordMatch dyn = MatchKeyword(line);
        if (dyn.ok && IEq(dyn.key, "BEGIN")) {
            std::string writer = dyn.value;
            const size_t sp = writer.find_first_of(" \t");
            if (sp != std::string::npos) writer = writer.substr(0, sp);
            st.is_block_begin = true;
            st.in_block = false;
            st.block_name = Lower(writer);
            BlockFrame f;
            f.name = Lower(writer);
            f.line = i;
            f.hash_start = dyn.key_start - 2;
            f.name_end = dyn.key_end + 1;
            f.dynamic = true;
            stack.push_back(f);
            continue;
        }
        if (dyn.ok && IEq(dyn.key, "END") && dyn.attr.empty()) {
            st.is_block_end = true;
            st.in_block = false;
            size_t found = stack.size();
            for (size_t k = stack.size(); k-- > 0;) {
                if (stack[k].dynamic) {
                    found = k;
                    break;
                }
            }
            if (found == stack.size()) {
                out.diags.push_back(Diag(i, dyn.key_start - 2, dyn.key_end + 1, OrgLspSeverity::Error, "unmatched-end",
                                         "`#+END:` closes no open `#+BEGIN:` dynamic block."));
            } else {
                st.block_name = stack[found].name;
                stack.resize(found);
            }
            continue;
        }

        const int level = OrgHeadlineLevel(line);
        if (level > 0) {
            if (drawer_open) {
                out.diags.push_back(Diag(drawer_line, drawer_col_start, drawer_col_end, OrgLspSeverity::Error, "unclosed-drawer",
                                         "Drawer :" + drawer_name + ": is never closed with `:END:` before the next headline."));
                drawer_open = false;
            }
            cur_headline = i;
            st.headline_line = i;
            prev_significant = i;
            continue;
        }

        std::string dname;
        size_t dstart = 0, dend = 0;
        const bool drawer_marker = MatchDrawer(line, &dname, &dstart, &dend);
        if (drawer_open) {
            if (drawer_marker && IEq(dname, "END")) {
                st.is_drawer_end = true;
                st.drawer_name = Upper(drawer_name);
                drawer_open = false;
                continue;
            }
            st.in_drawer = true;
            st.drawer_name = Upper(drawer_name);
            // A PROPERTIES drawer may hold nothing but `:KEY: value` lines
            // (`:KEY+:` appends). Other drawers hold arbitrary text, and a
            // LOGBOOK holds CLOCK lines and state notes, so neither is
            // checked here.
            if (drawer_is_properties && !AllBlank(line)) {
                size_t p = IndentEnd(line);
                bool well_formed = false;
                if (p < line.size() && line[p] == ':') {
                    size_t q = line.find(':', p + 1);
                    if (q != std::string::npos && q > p + 1) well_formed = true;
                }
                if (!well_formed) {
                    out.diags.push_back(DiagLine(i, line, OrgLspSeverity::Error, "bad-property",
                                                 "A :PROPERTIES: drawer holds only `:NAME: value` lines (or `:END:`)."));
                }
            }
            continue;
        }
        if (drawer_marker) {
            if (IEq(dname, "END")) {
                out.diags.push_back(Diag(i, dstart, dend, OrgLspSeverity::Error, "unmatched-drawer-end",
                                         "`:END:` here closes no open drawer."));
                continue;
            }
            st.is_drawer_begin = true;
            st.drawer_name = Upper(dname);
            drawer_open = true;
            drawer_name = dname;
            drawer_line = i;
            drawer_col_start = dstart;
            drawer_col_end = dend;
            drawer_is_properties = IEq(dname, "PROPERTIES");
            // Org only recognizes a property drawer directly under its
            // headline, optionally after the planning line. Anywhere else
            // it is inert text that silently stops being a property set --
            // exactly the kind of thing that looks right and does nothing.
            if (drawer_is_properties && prev_significant != i - 1) {
                out.diags.push_back(Diag(i, dstart, dend, OrgLspSeverity::Warning, "misplaced-properties",
                                         "A :PROPERTIES: drawer is only recognized directly under its headline "
                                         "(or right after that headline's planning line)."));
            }
            continue;
        }

        if (IsPlanningLine(line) && prev_significant == i - 1) prev_significant = i;
    }

    for (const BlockFrame &f : stack) {
        const std::string message = f.dynamic
                                        ? "`#+BEGIN: " + f.name + "` is never closed with `#+END:`."
                                        : "`#+begin_" + f.name + "` is never closed with `#+end_" + f.name + "`.";
        out.diags.push_back(Diag(f.line, f.hash_start, f.name_end, OrgLspSeverity::Error, "unclosed-block", message));
    }
    if (drawer_open) {
        out.diags.push_back(Diag(drawer_line, drawer_col_start, drawer_col_end, OrgLspSeverity::Error, "unclosed-drawer",
                                 "Drawer :" + drawer_name + ": is never closed with `:END:`."));
    }
    return out;
}

}  // namespace

namespace {

// --- Inline scanning --------------------------------------------------

// Bytes covered by a `=verbatim=` or `~code~` run (markers included). Org
// treats those literally, so nothing inside one is a link, a timestamp or
// a footnote -- without this mask, a help page writing `=<2026-13-01>=`
// to *show* a malformed timestamp gets told its timestamp is malformed.
// The PRE/POST/border rules are org's own (org-emphasis-regexp-components,
// already reproduced in org_doc.h for the renderer's sake).
/** @brief Marks every byte of `line` that sits inside a `=verbatim=` or `~code~` run. */
std::vector<bool> InlineLiteralMask(const std::string &line) {
    std::vector<bool> mask(line.size(), false);
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c != '=' && c != '~') continue;
        if (!OrgEmphasisPreOk(i == 0 ? '\0' : line[i - 1])) continue;
        if (i + 1 >= line.size() || OrgEmphasisBorderBlank(line[i + 1])) continue;
        for (size_t j = i + 1; j < line.size(); j++) {
            if (line[j] != c) continue;
            if (OrgEmphasisBorderBlank(line[j - 1])) continue;
            if (!OrgEmphasisPostOk(j + 1 < line.size() ? line[j + 1] : '\0')) continue;
            for (size_t k = i; k <= j; k++) mask[k] = true;
            i = j;
            break;
        }
    }
    return mask;
}

/** @brief Reports whether `s[at]` begins `count` ASCII digits. */
bool DigitsAt(const std::string &s, size_t at, size_t count) {
    if (at + count > s.size()) return false;
    for (size_t k = 0; k < count; k++) {
        if (std::isdigit(static_cast<unsigned char>(s[at + k])) == 0) return false;
    }
    return true;
}

/** @brief Number of days in a month, honoring the proleptic-Gregorian leap rule. */
int DaysInMonth(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

// English weekday names only. Org writes whatever the user's locale
// produces, so a name this doesn't recognize is skipped rather than
// reported -- flagging "Mi" (German Wednesday) as a wrong weekday would
// be worse than not checking at all.
/** @brief Maps an English weekday name/abbreviation to 0=Monday..6=Sunday, or -1 when unrecognized. */
int WeekdayIndex(const std::string &name) {
    static const char *const kAbbrev[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    static const char *const kFull[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    const std::string lower = Lower(name);
    for (int k = 0; k < 7; k++) {
        if (lower == kAbbrev[k] || lower == kFull[k]) return k;
    }
    return -1;
}

/** @brief Weekday of a proleptic-Gregorian date, 0=Monday..6=Sunday. */
int WeekdayOf(int year, int month, int day) {
    // Day 0 of OrgDayNumber's epoch (1970-01-01) was a Thursday = index 3.
    long long n = OrgDayNumber(year, month, day) + 3;
    long long w = n % 7;
    if (w < 0) w += 7;
    return static_cast<int>(w);
}

// One `<...>`/`[...]` run whose contents open with something shaped like
// a date. Anything else (a checkbox, a `[1/3]` cookie, a `<<target>>`, a
// `<%%(diary-sexp)>`, a stray `a < b`) is not a timestamp and is never
// reported on.
struct TimestampSpan {
    size_t start = 0, end = 0;  // half-open, markers included
    std::string body;           // between the markers
};

/** @brief Finds every `<...>`/`[...]` run on a line whose contents begin with a date-shaped token. */
std::vector<TimestampSpan> ScanTimestampSpans(const std::string &line, const std::vector<bool> &mask) {
    std::vector<TimestampSpan> out;
    for (size_t i = 0; i < line.size(); i++) {
        if (mask[i]) continue;
        const char open = line[i];
        if (open != '<' && open != '[') continue;
        const char close = open == '<' ? '>' : ']';
        // "YYYY-" is the cheapest unambiguous signal that this is a
        // timestamp rather than one of the many other bracket uses.
        if (!DigitsAt(line, i + 1, 4) || i + 5 >= line.size() || line[i + 5] != '-') continue;
        const size_t end = line.find(close, i + 1);
        if (end == std::string::npos) continue;
        TimestampSpan ts;
        ts.start = i;
        ts.end = end + 1;
        ts.body = line.substr(i + 1, end - i - 1);
        out.push_back(ts);
        i = end;
    }
    return out;
}

// --- Document-wide index ----------------------------------------------

// Everything an internal link can resolve to, plus the bookkeeping the
// duplicate-name check needs. Built once per lint/completion request.
struct DocIndex {
    std::set<std::string> headline_titles;
    std::set<std::string> custom_ids;
    std::set<std::string> names;
    std::set<std::string> radio_targets;
    std::set<std::string> tags;
    std::vector<std::string> todo_keywords, done_keywords;
};

/** @brief Extracts a `<<target>>` radio/internal target's label, if `line` holds one starting at `at`. */
bool RadioTargetAt(const std::string &line, size_t at, std::string *label, size_t *end) {
    if (at + 1 >= line.size() || line[at] != '<' || line[at + 1] != '<') return false;
    size_t p = at + 2;
    if (p < line.size() && line[p] == '<') p++;  // <<<radio>>>
    const size_t close = line.find(">>", p);
    if (close == std::string::npos || close == p) return false;
    std::string text = line.substr(p, close - p);
    while (!text.empty() && text.back() == '>') text.pop_back();
    if (text.empty()) return false;
    if (label) *label = text;
    if (end) *end = close + 2;
    return true;
}

/** @brief Collects every internal link target, tag and TODO keyword the document defines. */
DocIndex BuildDocIndex(const std::vector<std::string> &lines, const std::vector<LineState> &states) {
    DocIndex ix;
    const OrgOutline outline = ParseOrgOutline(lines);
    ix.todo_keywords = outline.todo_keywords;
    ix.done_keywords = outline.done_keywords;
    for (const OrgHeadline &h : outline.headlines) {
        if (!h.title.empty()) ix.headline_titles.insert(Trim(h.title));
        for (const std::string &t : h.tags) ix.tags.insert(t);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        if (states[i].in_literal) continue;
        if (states[i].in_drawer) {
            // `:CUSTOM_ID: value` inside a PROPERTIES drawer.
            const size_t p = IndentEnd(line);
            if (p < line.size() && line[p] == ':') {
                const size_t q = line.find(':', p + 1);
                if (q != std::string::npos && IEq(line.substr(p + 1, q - p - 1), "CUSTOM_ID")) {
                    const std::string v = Trim(line.substr(q + 1));
                    if (!v.empty()) ix.custom_ids.insert(v);
                }
            }
            continue;
        }
        const KeywordMatch km = MatchKeyword(line);
        if (km.ok && IEq(km.key, "NAME") && !km.value.empty()) ix.names.insert(km.value);
        if (km.ok && (IEq(km.key, "FILETAGS") || IEq(km.key, "TAGS"))) {
            std::string tok;
            for (char c : km.value + ":") {
                if (c == ':' || c == ' ' || c == '\t') {
                    if (!tok.empty()) ix.tags.insert(tok);
                    tok.clear();
                } else {
                    tok += c;
                }
            }
        }
        const std::vector<bool> mask = InlineLiteralMask(line);
        for (size_t c = 0; c + 1 < line.size(); c++) {
            if (mask[c]) continue;
            std::string label;
            size_t end = 0;
            if (RadioTargetAt(line, c, &label, &end)) {
                ix.radio_targets.insert(label);
                c = end - 1;
            }
        }
    }
    return ix;
}

// --- Did-you-mean -----------------------------------------------------

/** @brief Returns the vocabulary entry within `limit` edits of `word` (case-insensitively), or "" when none is close. */
std::string ClosestName(const std::string &word, const std::vector<OrgLspVocabEntry> &vocab, int limit) {
    const std::string needle = Lower(word);
    std::string best;
    int best_dist = limit + 1;
    for (const OrgLspVocabEntry &e : vocab) {
        const int d = EditDistance(needle, Lower(e.name), limit);
        if (d < best_dist) {
            best_dist = d;
            best = e.name;
        }
    }
    return best_dist <= limit ? best : std::string();
}

/** @brief Returns the string within `limit` edits of `word` (case-insensitively), or "" when none is close. */
std::string ClosestOf(const std::string &word, const std::vector<std::string> &pool, int limit) {
    const std::string needle = Lower(word);
    std::string best;
    int best_dist = limit + 1;
    for (const std::string &cand : pool) {
        const int d = EditDistance(needle, Lower(cand), limit);
        if (d < best_dist) {
            best_dist = d;
            best = cand;
        }
    }
    return best_dist <= limit ? best : std::string();
}

/** @brief Appends a " Did you mean `X`?" clause when `suggestion` is non-empty. */
std::string WithSuggestion(std::string message, const std::string &suggestion, const std::string &prefix = "") {
    if (suggestion.empty()) return message;
    return message + " Did you mean `" + prefix + suggestion + "`?";
}

}  // namespace

namespace {

// --- Per-construct checks ---------------------------------------------

/** @brief Validates one `<...>`/`[...]` timestamp's date, weekday, time-of-day and repeater/warning cookies. */
void CheckTimestamp(int row, const TimestampSpan &ts, std::vector<OrgLspDiagnostic> *diags) {
    const std::string &b = ts.body;
    // Shape: YYYY-MM-DD [Day] [HH:MM[-HH:MM]] [repeater] [warning]
    // ScanTimestampSpans already guaranteed four digits followed by '-'.
    const int year = std::stoi(b.substr(0, 4));
    size_t p = 5;
    const size_t mstart = p;
    while (p < b.size() && std::isdigit(static_cast<unsigned char>(b[p])) != 0) p++;
    const size_t mlen = p - mstart;
    if (mlen == 0 || p >= b.size() || b[p] != '-') {
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-date",
                              "Malformed timestamp: expected `<YYYY-MM-DD Day>`."));
        return;
    }
    const int month = std::stoi(b.substr(mstart, mlen));
    p++;
    const size_t dstart = p;
    while (p < b.size() && std::isdigit(static_cast<unsigned char>(b[p])) != 0) p++;
    const size_t dlen = p - dstart;
    if (dlen == 0) {
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-date",
                              "Malformed timestamp: expected `<YYYY-MM-DD Day>`."));
        return;
    }
    const int day = std::stoi(b.substr(dstart, dlen));
    if (mlen != 2 || dlen != 2) {
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Warning, "unpadded-date",
                              "Org writes timestamps zero-padded (`<YYYY-MM-DD>`); agenda sorting compares them as text."));
    }
    if (month < 1 || month > 12) {
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-date",
                              "Month " + std::to_string(month) + " is out of range (1-12)."));
        return;
    }
    if (day < 1 || day > DaysInMonth(year, month)) {
        static const char *const kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                              "July",    "August",   "September", "October", "November", "December"};
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-date",
                              std::string(kMonths[month - 1]) + " " + std::to_string(year) + " has no day " +
                                  std::to_string(day) + "."));
        return;
    }
    // Remaining tokens: an optional weekday name, an optional time (or
    // time range), then any number of repeater/warning cookies.
    std::vector<std::string> tokens;
    std::string tok;
    for (size_t k = p; k <= b.size(); k++) {
        const char c = k < b.size() ? b[k] : ' ';
        if (c == ' ' || c == '\t') {
            if (!tok.empty()) tokens.push_back(tok);
            tok.clear();
        } else {
            tok += c;
        }
    }
    for (size_t t = 0; t < tokens.size(); t++) {
        const std::string &word = tokens[t];
        if (t == 0) {
            const int want = WeekdayIndex(word);
            if (want >= 0) {
                const int have = WeekdayOf(year, month, day);
                if (want != have) {
                    static const char *const kNames[] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                                         "Friday",   "Saturday", "Sunday"};
                    diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Warning, "wrong-weekday",
                                          "That date is a " + std::string(kNames[have]) + ", not a " + word + "."));
                }
                continue;
            }
            // An all-letters first token that this does not recognize is a
            // weekday name in some other locale -- org writes whatever the
            // user's locale produces ("Mo", "lun.", "\u6708"). Accepted without
            // comment: it cannot be checked, and falling through to the
            // repeater classification below would report every non-English
            // org file's every timestamp as malformed.
            bool alpha_only = true;
            for (char ch : word) {
                if (std::isalpha(static_cast<unsigned char>(ch)) == 0 && ch != '.') alpha_only = false;
            }
            if (alpha_only) continue;
            // Otherwise it is a time or a repeater that simply came before
            // any weekday name; classify it below.
        }
        // Time of day, optionally a range: HH:MM or HH:MM-HH:MM.
        if (std::isdigit(static_cast<unsigned char>(word[0])) != 0 && word.find(':') != std::string::npos) {
            bool bad = false;
            bool reported = false;  // the out-of-range report below is its own diagnostic
            size_t q = 0;
            while (q < word.size() && !bad) {
                const size_t hs = q;
                while (q < word.size() && std::isdigit(static_cast<unsigned char>(word[q])) != 0) q++;
                if (q == hs || q >= word.size() || word[q] != ':') {
                    bad = true;
                    break;
                }
                const int hour = std::stoi(word.substr(hs, q - hs));
                q++;
                const size_t ms2 = q;
                while (q < word.size() && std::isdigit(static_cast<unsigned char>(word[q])) != 0) q++;
                if (q == ms2) {
                    bad = true;
                    break;
                }
                const int minute = std::stoi(word.substr(ms2, q - ms2));
                if (hour > 24 || minute > 59) {
                    diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-time",
                                          "`" + word + "` is not a valid time of day."));
                    bad = true;
                    reported = true;
                    break;
                }
                if (q < word.size() && (word[q] == '-' || word[q] == '+')) q++;
                else break;
            }
            // Trailing junk after a time that otherwise parsed (`9:0x`) is
            // just as malformed as one that never parsed at all.
            if (!bad && q != word.size()) bad = true;
            if (bad && !reported) {
                diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Error, "bad-time",
                                      "`" + word + "` is not a valid time of day."));
            }
            continue;
        }
        // Repeater (`+1w`, `++2d`, `.+1m`) or warning period (`-3d`).
        size_t q = 0;
        if (q < word.size() && (word[q] == '.' || word[q] == '+')) q++;
        if (q < word.size() && (word[q] == '+' || word[q] == '-')) q++;
        const size_t nstart = q;
        while (q < word.size() && std::isdigit(static_cast<unsigned char>(word[q])) != 0) q++;
        const bool has_digits = q > nstart;
        const bool has_unit = q + 1 == word.size() && (word[q] == 'h' || word[q] == 'd' || word[q] == 'w' ||
                                                       word[q] == 'm' || word[q] == 'y');
        if (nstart > 0 && has_digits && has_unit) continue;
        // `=> 1:30` effort totals on CLOCK lines live outside the
        // brackets, so anything left here really is unexpected.
        diags->push_back(Diag(row, ts.start, ts.end, OrgLspSeverity::Warning, "bad-timestamp-part",
                              "`" + word + "` is not a weekday, a time of day or a repeater/warning cookie."));
    }
}

// Where a link target points. Kept separate from the check itself so
// OrgLspDefinition can reuse the same classification.
enum class LinkKind { External, FilePath, HeadlineSearch, CustomId, Fuzzy };

/** @brief Classifies a link target and strips a `::search` suffix / `file:` scheme from it. */
LinkKind ClassifyLink(const std::string &raw, std::string *payload, std::string *search) {
    std::string t = raw;
    if (search) search->clear();
    static const char *const kExternalSchemes[] = {"http:",  "https:", "mailto:", "news:",   "ftp:",  "doi:",
                                                   "elisp:", "shell:", "id:",     "info:",   "help:", "man:",
                                                   "irc:",   "rmail:", "gnus:",   "bbdb:",   "bibtex:", "cite:",
                                                   "attachment:", "docview:", "eww:", "w3m:"};
    for (const char *scheme : kExternalSchemes) {
        if (IStartsWith(t, scheme)) {
            if (payload) *payload = t;
            return LinkKind::External;
        }
    }
    bool explicit_file = false;
    if (IStartsWith(t, "file:")) {
        t = t.substr(5);
        explicit_file = true;
    }
    if (!t.empty() && t[0] == '*') {
        if (payload) *payload = Trim(t.substr(1));
        return LinkKind::HeadlineSearch;
    }
    if (!t.empty() && t[0] == '#') {
        if (payload) *payload = Trim(t.substr(1));
        return LinkKind::CustomId;
    }
    const size_t sep = t.find("::");
    if (sep != std::string::npos) {
        if (search) *search = t.substr(sep + 2);
        t = t.substr(0, sep);
    }
    if (explicit_file || t.find('/') != std::string::npos || IStartsWith(t, "./") || IStartsWith(t, "../")) {
        if (payload) *payload = t;
        return LinkKind::FilePath;
    }
    // A bare `name.ext` is a file too (org resolves it relative to the
    // document), but a bare word with no dot is a fuzzy internal search.
    if (t.find('.') != std::string::npos && t.find(' ') == std::string::npos) {
        if (payload) *payload = t;
        return LinkKind::FilePath;
    }
    if (payload) *payload = t;
    return LinkKind::Fuzzy;
}

/** @brief Resolves a link's file path against the document's directory, or "" when it cannot be resolved locally. */
std::string ResolveLinkPath(const std::string &path, const OrgLspOptions &opts) {
    if (path.empty() || path[0] == '~') return "";  // no shell expansion here
    if (path[0] == '/') return path;
    if (opts.doc_dir.empty()) return "";
    return opts.doc_dir + "/" + path;
}

/** @brief Reports whether a path exists on disk, swallowing every filesystem error as "cannot tell". */
bool PathExists(const std::string &path) {
    if (path.empty()) return true;  // "cannot tell" must never produce a diagnostic
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

/** @brief Validates every bracket link on a line: closure, empty targets, and whether the target resolves. */
void CheckLinks(int row, const std::string &line, const std::vector<bool> &mask, const DocIndex &ix,
                const OrgLspOptions &opts, std::vector<OrgLspDiagnostic> *diags) {
    for (size_t i = 0; i + 1 < line.size(); i++) {
        if (mask[i] || line[i] != '[' || line[i + 1] != '[') continue;
        const size_t start = i;
        const size_t close = line.find("]]", i + 2);
        if (close == std::string::npos) {
            diags->push_back(Diag(row, start, line.size(), OrgLspSeverity::Error, "unclosed-link",
                                  "This link is never closed with `]]`."));
            return;
        }
        const size_t end = close + 2;
        i = close + 1;  // resume scanning after this link
        const std::string body = line.substr(start + 2, close - start - 2);
        // `[[target][description]]`: a description may itself contain a
        // `]`, so the split is on the first `][` only.
        const size_t desc = body.find("][");
        const std::string target = Trim(desc == std::string::npos ? body : body.substr(0, desc));
        if (target.empty()) {
            diags->push_back(Diag(row, start, end, OrgLspSeverity::Error, "empty-link", "This link has an empty target."));
            continue;
        }
        // A `{{{macro}}}` expands at export time to something this cannot
        // know, so such a target is never resolved.
        if (target.find("{{{") != std::string::npos) continue;
        std::string payload, search;
        const LinkKind kind = ClassifyLink(target, &payload, &search);
        switch (kind) {
            case LinkKind::External:
                break;
            case LinkKind::FilePath: {
                if (!opts.check_files) break;
                const std::string resolved = ResolveLinkPath(payload, opts);
                if (!resolved.empty() && !PathExists(resolved)) {
                    diags->push_back(Diag(row, start, end, OrgLspSeverity::Warning, "missing-file",
                                          "Link target `" + payload + "` does not exist."));
                }
                break;
            }
            case LinkKind::HeadlineSearch: {
                if (ix.headline_titles.find(payload) == ix.headline_titles.end()) {
                    const std::vector<std::string> pool(ix.headline_titles.begin(), ix.headline_titles.end());
                    diags->push_back(Diag(row, start, end, OrgLspSeverity::Warning, "unresolved-link",
                                          WithSuggestion("No headline titled `" + payload + "` in this file.",
                                                         ClosestOf(payload, pool, 2), "*")));
                }
                break;
            }
            case LinkKind::CustomId:
                if (ix.custom_ids.find(payload) == ix.custom_ids.end()) {
                    diags->push_back(Diag(row, start, end, OrgLspSeverity::Warning, "unresolved-link",
                                          "No `:CUSTOM_ID: " + payload + "` property in this file."));
                }
                break;
            case LinkKind::Fuzzy:
                if (ix.names.find(payload) == ix.names.end() && ix.radio_targets.find(payload) == ix.radio_targets.end() &&
                    ix.headline_titles.find(payload) == ix.headline_titles.end() &&
                    ix.custom_ids.find(payload) == ix.custom_ids.end()) {
                    diags->push_back(Diag(row, start, end, OrgLspSeverity::Warning, "unresolved-link",
                                          "Nothing in this file is named `" + payload +
                                              "` (no `#+NAME:`, `<<target>>`, `:CUSTOM_ID:` or headline)."));
                }
                break;
        }
    }
}

}  // namespace

namespace {

// --- Babel header arguments -------------------------------------------

// One `:key value` pair off a `#+begin_src`/`#+HEADER:`/`#+PROPERTY:`
// line. Values run to the next ` :key` at quote depth zero, so
// `:file "a :b.png"` and `:var x="1 2"` survive intact.
struct HeaderArg {
    std::string key;
    size_t key_start = 0, key_end = 0;  // spans the `:key`, colon included
    std::string value;                  // trimmed
    size_t value_start = 0, value_end = 0;
};

/** @brief Splits the `:key value` pairs out of a babel header-argument string starting at `from`. */
std::vector<HeaderArg> ParseHeaderArgs(const std::string &line, size_t from) {
    std::vector<HeaderArg> out;
    size_t i = from;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= line.size()) break;
        if (line[i] != ':' || i + 1 >= line.size() || std::isalpha(static_cast<unsigned char>(line[i + 1])) == 0) {
            // A positional token (the language, or a `-n`/`+r` switch).
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
            continue;
        }
        HeaderArg arg;
        arg.key_start = i;
        size_t j = i + 1;
        while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) != 0 || line[j] == '-' || line[j] == '_')) j++;
        arg.key_end = j;
        arg.key = line.substr(i + 1, j - i - 1);
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) j++;
        arg.value_start = j;
        char quote = '\0';
        size_t k = j;
        while (k < line.size()) {
            const char c = line[k];
            if (quote != '\0') {
                if (c == '\\' && k + 1 < line.size()) k++;
                else if (c == quote) quote = '\0';
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if ((c == ' ' || c == '\t') && k + 2 < line.size() && line[k + 1] == ':' &&
                       std::isalpha(static_cast<unsigned char>(line[k + 2])) != 0) {
                break;
            }
            k++;
        }
        arg.value_end = k;
        arg.value = Trim(line.substr(j, k - j));
        out.push_back(arg);
        i = k;
    }
    return out;
}

/** @brief Looks up an enumerated header argument's allowed values, or nullptr when the key takes free-form text. */
const std::vector<const char *> *HeaderArgValues(const std::string &key) {
    const auto it = kHeaderArgValues.find(Lower(key));
    return it == kHeaderArgValues.end() ? nullptr : &it->second;
}

/** @brief Reports whether a header-argument name is one this server knows. */
bool KnownHeaderArg(const std::string &key) {
    for (const OrgLspVocabEntry &e : kHeaderArgs) {
        if (IEq(key, e.name)) return true;
    }
    return false;
}

/** @brief Reports whether a `#+begin_src` language tag names something babel commonly handles. */
bool KnownBabelLang(const std::string &lang) {
    for (const OrgLspVocabEntry &e : kBabelLangs) {
        if (IEq(lang, e.name)) return true;
    }
    for (const char *other : kOtherKnownLangs) {
        if (IEq(lang, other)) return true;
    }
    return false;
}

/** @brief Validates a babel header-argument list: unknown names, and unknown values for the enumerated ones. */
void CheckHeaderArgs(int row, const std::string &line, size_t from, std::vector<OrgLspDiagnostic> *diags) {
    for (const HeaderArg &arg : ParseHeaderArgs(line, from)) {
        if (!KnownHeaderArg(arg.key)) {
            diags->push_back(Diag(row, arg.key_start, arg.key_end, OrgLspSeverity::Hint, "unknown-header-arg",
                                  WithSuggestion("`:" + arg.key + "` is not a header argument org or mep defines.",
                                                 ClosestName(arg.key, kHeaderArgs, 2), ":")));
            continue;
        }
        if (IEq(arg.key, "var") && arg.value.find('=') == std::string::npos && !arg.value.empty()) {
            diags->push_back(Diag(row, arg.value_start, arg.value_end, OrgLspSeverity::Warning, "bad-var",
                                  "`:var` binds a variable: write `:var name=value`."));
            continue;
        }
        const std::vector<const char *> *allowed = HeaderArgValues(arg.key);
        if (allowed == nullptr || arg.value.empty()) continue;
        // `:results` takes several words at once (`output verbatim
        // replace`); every other enumerated key takes exactly one, but
        // checking word by word is correct for both.
        size_t w = 0;
        while (w < arg.value.size()) {
            while (w < arg.value.size() && (arg.value[w] == ' ' || arg.value[w] == '\t')) w++;
            const size_t ws = w;
            while (w < arg.value.size() && arg.value[w] != ' ' && arg.value[w] != '\t') w++;
            if (w == ws) break;
            const std::string word = arg.value.substr(ws, w - ws);
            bool ok = false;
            std::vector<std::string> pool;
            for (const char *v : *allowed) {
                pool.emplace_back(v);
                if (IEq(word, v)) ok = true;
            }
            if (!ok) {
                diags->push_back(Diag(row, arg.value_start + ws, arg.value_start + w, OrgLspSeverity::Warning,
                                      "unknown-header-value",
                                      WithSuggestion("`" + word + "` is not a value `:" + arg.key + "` accepts.",
                                                     ClosestOf(word, pool, 2))));
            }
        }
    }
}

/** @brief Validates a `#+begin_src`/special-block opening line: its language and its header arguments. */
void CheckBlockBegin(int row, const std::string &line, const BlockMatch &bm, std::vector<OrgLspDiagnostic> *diags) {
    const std::string name = Lower(bm.name);
    if (name == "src") {
        if (bm.args.empty()) {
            diags->push_back(Diag(row, bm.name_start, bm.name_end, OrgLspSeverity::Warning, "src-no-language",
                                  "This source block names no language, so babel cannot evaluate or highlight it."));
            return;
        }
        size_t lang_end = bm.args_start;
        while (lang_end < line.size() && line[lang_end] != ' ' && line[lang_end] != '\t') lang_end++;
        const std::string lang = line.substr(bm.args_start, lang_end - bm.args_start);
        if (!lang.empty() && lang[0] == ':') {
            diags->push_back(Diag(row, bm.name_start, bm.name_end, OrgLspSeverity::Warning, "src-no-language",
                                  "This source block names no language (the first token is a header argument)."));
        } else if (!KnownBabelLang(lang)) {
            diags->push_back(Diag(row, bm.args_start, lang_end, OrgLspSeverity::Information, "unknown-src-language",
                                  WithSuggestion("mep's babel has no `" + lang + "` language; this block cannot be evaluated.",
                                                 ClosestName(lang, kBabelLangs, 2))));
        }
        CheckHeaderArgs(row, line, lang_end, diags);
        return;
    }
    if (name == "export") {
        if (bm.args.empty()) {
            diags->push_back(Diag(row, bm.name_start, bm.name_end, OrgLspSeverity::Warning, "export-no-backend",
                                  "An export block names its backend: `#+begin_export html`."));
        }
        return;
    }
    // Any other name is a legal "special block" (org wraps it in an
    // environment of the same name), so this only fires when the name is
    // within typo distance of a block org gives real meaning to.
    bool known = false;
    for (const OrgLspVocabEntry &e : kBlocks) {
        if (name == e.name) known = true;
    }
    if (known) return;
    const std::string near = ClosestName(name, kBlocks, 1);
    if (!near.empty()) {
        diags->push_back(Diag(row, bm.name_start, bm.name_end, OrgLspSeverity::Hint, "unknown-block",
                              WithSuggestion("`" + bm.name + "` is exported as a plain special block.", near, "begin_")));
    }
}

/** @brief Validates a headline's priority cookie and trailing tag group, and reports a skipped outline level. */
void CheckHeadline(int row, const std::string &line, int level, int prev_level, const DocIndex &ix,
                   std::vector<OrgLspDiagnostic> *diags) {
    if (prev_level > 0 && level > prev_level + 1) {
        diags->push_back(Diag(row, 0, static_cast<size_t>(level), OrgLspSeverity::Hint, "level-skip",
                              "Outline level jumps from " + std::to_string(prev_level) + " to " + std::to_string(level) +
                                  "; org treats this as a child, but folding and export numbering both skip a level."));
    }
    // Priority cookie: only ever valid directly after the stars, or
    // directly after the TODO keyword.
    size_t p = static_cast<size_t>(level);
    while (p < line.size() && line[p] == ' ') p++;
    size_t word_end = p;
    while (word_end < line.size() && line[word_end] != ' ') word_end++;
    const std::string first = line.substr(p, word_end - p);
    for (const std::vector<std::string> *kws : {&ix.todo_keywords, &ix.done_keywords}) {
        for (const std::string &kw : *kws) {
            if (first == kw) {
                p = word_end;
                while (p < line.size() && line[p] == ' ') p++;
            }
        }
    }
    if (p + 1 < line.size() && line[p] == '[' && line[p + 1] == '#') {
        const size_t close = line.find(']', p);
        const size_t len = close == std::string::npos ? 0 : close - p - 2;
        if (close == std::string::npos || len != 1) {
            diags->push_back(Diag(row, p, close == std::string::npos ? line.size() : close + 1, OrgLspSeverity::Warning,
                                  "bad-priority", "A priority cookie is one character: `[#A]`."));
        }
    }
    // Trailing `:tag1:tag2:` group. Only a group that is already
    // tag-shaped (or tag-shaped apart from spaces) is reported on --
    // otherwise an ordinary headline ending in a colon gets called a
    // malformed tag list.
    size_t e = line.size();
    while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
    if (e == 0 || line[e - 1] != ':') return;
    size_t s = e - 1;
    bool found_start = false;
    while (s > 0) {
        s--;
        if (line[s] == ':' && (line[s - 1] == ' ' || line[s - 1] == '\t')) {
            found_start = true;
            break;
        }
    }
    if (!found_start || s + 1 >= e) return;
    const std::string group = line.substr(s + 1, e - s - 2);  // between the outer colons
    bool tag_chars = true, has_space = false, empty_segment = group.empty();
    std::string segment;
    for (char c : group + ":") {
        if (c == ':') {
            if (segment.empty()) empty_segment = true;
            segment.clear();
            continue;
        }
        if (c == ' ' || c == '\t') {
            has_space = true;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '@' && c != '#' && c != '%') tag_chars = false;
        segment += c;
    }
    if (!tag_chars) return;  // not a tag group at all, just prose
    if (has_space) {
        diags->push_back(Diag(row, s, e, OrgLspSeverity::Warning, "bad-tags",
                              "Tags may not contain spaces; org will not recognize this as a tag list."));
    } else if (empty_segment) {
        diags->push_back(Diag(row, s, e, OrgLspSeverity::Warning, "bad-tags",
                              "Empty tag in this tag list (`::` has nothing between the colons)."));
    }
}

/** @brief Validates a `#+TODO:`/`#+SEQ_TODO:`/`#+TYP_TODO:` keyword sequence. */
void CheckTodoLine(int row, const KeywordMatch &km, std::vector<OrgLspDiagnostic> *diags) {
    if (km.value.empty()) {
        diags->push_back(Diag(row, km.key_start, km.key_end, OrgLspSeverity::Warning, "bad-todo-line",
                              "`#+" + km.key + ":` needs at least one keyword, e.g. `TODO | DONE`."));
        return;
    }
    int bars = 0;
    size_t i = 0;
    while (i < km.value.size()) {
        while (i < km.value.size() && (km.value[i] == ' ' || km.value[i] == '\t')) i++;
        const size_t start = i;
        while (i < km.value.size() && km.value[i] != ' ' && km.value[i] != '\t') i++;
        if (i == start) break;
        const std::string tok = km.value.substr(start, i - start);
        if (tok == "|") {
            bars++;
            continue;
        }
        // `NEXT(n)` / `WAIT(w@/!)`: the fast-select/logging suffix.
        std::string bare = tok;
        const size_t paren = bare.find('(');
        if (paren != std::string::npos) {
            if (bare.back() != ')') {
                diags->push_back(Diag(row, km.value_start + start, km.value_start + i, OrgLspSeverity::Warning,
                                      "bad-todo-line", "Unclosed `(` in TODO keyword `" + tok + "`."));
                continue;
            }
            bare = bare.substr(0, paren);
        }
        if (bare.empty()) {
            diags->push_back(Diag(row, km.value_start + start, km.value_start + i, OrgLspSeverity::Warning, "bad-todo-line",
                                  "A TODO keyword needs a name before its `(...)` fast-select key."));
            continue;
        }
        for (char c : bare) {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_' && c != '-') {
                diags->push_back(Diag(row, km.value_start + start, km.value_start + i, OrgLspSeverity::Warning,
                                      "bad-todo-line",
                                      "TODO keyword `" + bare + "` contains characters org does not allow in one."));
                break;
            }
        }
    }
    if (bars > 1) {
        diags->push_back(Diag(row, km.value_start, km.value_start + km.value.size(), OrgLspSeverity::Warning, "bad-todo-line",
                              "A TODO sequence has at most one `|` separating the not-done from the done keywords."));
    }
}

/** @brief Validates a `#+OPTIONS:` line's `key:value` switches. */
void CheckOptionsLine(int row, const KeywordMatch &km, std::vector<OrgLspDiagnostic> *diags) {
    size_t i = 0;
    while (i < km.value.size()) {
        while (i < km.value.size() && (km.value[i] == ' ' || km.value[i] == '\t')) i++;
        const size_t start = i;
        while (i < km.value.size() && km.value[i] != ' ' && km.value[i] != '\t') i++;
        if (i == start) break;
        const std::string tok = km.value.substr(start, i - start);
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) {
            diags->push_back(Diag(row, km.value_start + start, km.value_start + i, OrgLspSeverity::Warning, "bad-option",
                                  "`#+OPTIONS:` switches are written `key:value`, e.g. `toc:nil`."));
            continue;
        }
        const std::string key = tok.substr(0, colon + 1);
        bool known = false;
        std::vector<std::string> pool;
        for (const OrgLspVocabEntry &e : kOptions) {
            pool.emplace_back(e.name);
            if (key == e.name) known = true;
        }
        if (!known) {
            diags->push_back(Diag(row, km.value_start + start, km.value_start + start + colon + 1, OrgLspSeverity::Hint,
                                  "unknown-option",
                                  WithSuggestion("`" + key + "` is not an export option org defines.",
                                                 ClosestOf(key, pool, 1))));
        }
    }
}

/** @brief Validates a `#+STARTUP:` line's words. */
void CheckStartupLine(int row, const KeywordMatch &km, std::vector<OrgLspDiagnostic> *diags) {
    size_t i = 0;
    while (i < km.value.size()) {
        while (i < km.value.size() && (km.value[i] == ' ' || km.value[i] == '\t')) i++;
        const size_t start = i;
        while (i < km.value.size() && km.value[i] != ' ' && km.value[i] != '\t') i++;
        if (i == start) break;
        const std::string tok = km.value.substr(start, i - start);
        bool known = false;
        for (const OrgLspVocabEntry &e : kStartup) {
            if (IEq(tok, e.name)) known = true;
        }
        if (!known) {
            diags->push_back(Diag(row, km.value_start + start, km.value_start + i, OrgLspSeverity::Hint, "unknown-startup",
                                  WithSuggestion("`" + tok + "` is not a `#+STARTUP:` word org defines.",
                                                 ClosestName(tok, kStartup, 2))));
        }
    }
}

/** @brief Reports whether a keyword name is one of the open-ended backend-prefixed families (`ATTR_*`, `LATEX_*`, ...). */
bool HasOpenEndedKeywordPrefix(const std::string &key) {
    const std::string upper = Upper(key);
    for (const char *prefix : kKeywordPrefixes) {
        if (upper.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

std::vector<OrgLspDiagnostic> OrgLspDiagnostics(const std::vector<std::string> &lines, const OrgLspOptions &opts) {
    StructureResult structure = ScanStructure(lines);
    std::vector<OrgLspDiagnostic> diags = std::move(structure.diags);
    const DocIndex ix = BuildDocIndex(lines, structure.states);

    // Footnotes are the one check that cannot be decided line by line:
    // a reference is only dangling once the *whole* document has been
    // read, so both sides are collected here and reconciled at the end.
    std::map<std::string, std::vector<std::pair<int, size_t>>> fn_defs, fn_refs;
    std::map<std::string, int> name_first_line;
    int prev_level = 0;
    bool planning_ok = false;  // the previous line was a headline or a planning line

    for (size_t idx = 0; idx < lines.size(); idx++) {
        const std::string &line = lines[idx];
        const int row = static_cast<int>(idx);
        const LineState &st = structure.states[idx];
        if (st.in_literal) {
            planning_ok = false;
            continue;
        }

        const int level = OrgHeadlineLevel(line);
        if (level > 0) {
            CheckHeadline(row, line, level, prev_level, ix, &diags);
            prev_level = level;
        } else if (st.is_block_begin) {
            const BlockMatch bm = MatchBlock(line);
            if (bm.ok) CheckBlockBegin(row, line, bm, &diags);
        } else if (!st.is_block_end && !st.in_drawer && !st.is_drawer_begin && !st.is_drawer_end) {
            // `#+end_src` and `#+END:` are block markers ScanStructure
            // already accounted for; running the keyword checks over them
            // would call every closer a keyword missing its colon.
            const KeywordMatch km = MatchKeyword(line);
            if (km.ok) {
                const std::string key = Upper(km.key);
                if (key == "TODO" || key == "SEQ_TODO" || key == "TYP_TODO") {
                    CheckTodoLine(row, km, &diags);
                } else if (key == "OPTIONS") {
                    CheckOptionsLine(row, km, &diags);
                } else if (key == "STARTUP") {
                    CheckStartupLine(row, km, &diags);
                } else if (key == "HEADER" || key == "PROPERTY") {
                    // `#+PROPERTY: header-args :results output` -- the
                    // header arguments start after the property name.
                    size_t from = km.value_start;
                    if (key == "PROPERTY") {
                        while (from < line.size() && (line[from] == ' ' || line[from] == '\t')) from++;
                        while (from < line.size() && line[from] != ' ' && line[from] != '\t') from++;
                    }
                    CheckHeaderArgs(row, line, from, &diags);
                } else if (key == "NAME") {
                    if (!km.value.empty()) {
                        const auto it = name_first_line.find(km.value);
                        if (it != name_first_line.end()) {
                            diags.push_back(Diag(row, km.value_start, line.size(), OrgLspSeverity::Warning, "duplicate-name",
                                                 "`#+NAME: " + km.value + "` is already used on line " +
                                                     std::to_string(it->second + 1) + "; `[[" + km.value +
                                                     "]]` and `#+CALL:` resolve to the first one."));
                        } else {
                            name_first_line[km.value] = row;
                        }
                    }
                } else if (key == "INCLUDE") {
                    if (opts.check_files && !km.value.empty()) {
                        std::string path = km.value;
                        if (path[0] == '"') {
                            const size_t close = path.find('"', 1);
                            path = close == std::string::npos ? path.substr(1) : path.substr(1, close - 1);
                        } else {
                            const size_t sp = path.find_first_of(" \t");
                            if (sp != std::string::npos) path = path.substr(0, sp);
                        }
                        const size_t sep = path.find("::");
                        if (sep != std::string::npos) path = path.substr(0, sep);
                        const std::string resolved = ResolveLinkPath(path, opts);
                        if (!resolved.empty() && !PathExists(resolved)) {
                            diags.push_back(Diag(row, km.value_start, line.size(), OrgLspSeverity::Warning, "missing-file",
                                                 "`#+INCLUDE:` target `" + path + "` does not exist."));
                        }
                    }
                } else if (!HasOpenEndedKeywordPrefix(km.key)) {
                    bool known = false;
                    for (const OrgLspVocabEntry &e : kKeywords) {
                        if (key == e.name) known = true;
                    }
                    if (!known) {
                        const std::string near = ClosestName(km.key, kKeywords, 2);
                        diags.push_back(Diag(row, km.key_start - 2, km.key_end, near.empty() ? OrgLspSeverity::Hint
                                                                                            : OrgLspSeverity::Warning,
                                             "unknown-keyword",
                                             WithSuggestion("`#+" + km.key + ":` is not a keyword org defines; it is "
                                                            "ignored on export.",
                                                            near, "#+")));
                    }
                }
            } else if (km.has_prefix && !km.key.empty() && !IsCommentLine(line)) {
                diags.push_back(Diag(row, km.key_start - 2, km.key_end, OrgLspSeverity::Warning, "keyword-no-colon",
                                     "`#+" + km.key + "` is missing its `:`; org reads this line as ordinary text."));
            }
        }

        // A planning line only counts where org looks for one. Written
        // anywhere else it silently does nothing, which is exactly the
        // kind of quiet no-op worth a diagnostic -- but only when it
        // carries a timestamp, so prose like "DEADLINE: next Friday" in a
        // paragraph is left alone.
        if (level == 0 && IsPlanningLine(line) && !planning_ok && !st.in_drawer && !st.in_block &&
            line.find('<') != std::string::npos) {
            diags.push_back(DiagLine(row, line, OrgLspSeverity::Warning, "misplaced-planning",
                                     "A SCHEDULED/DEADLINE/CLOSED line is only recognized directly under its headline."));
        }

        const std::vector<bool> mask = InlineLiteralMask(line);
        for (const TimestampSpan &ts : ScanTimestampSpans(line, mask)) CheckTimestamp(row, ts, &diags);
        if (!st.in_drawer) CheckLinks(row, line, mask, ix, opts, &diags);

        // Footnotes. A definition is `[fn:label]` starting in column 0;
        // the same syntax anywhere else is a reference. `[fn:label:text]`
        // and `[fn::text]` are inline definitions, which also count as a
        // reference at that spot.
        for (size_t i = 0; i + 3 < line.size(); i++) {
            if (mask[i] || line[i] != '[' || line.compare(i, 4, "[fn:") != 0) continue;
            size_t j = i + 4;
            while (j < line.size() && line[j] != ']' && line[j] != ':') j++;
            if (j >= line.size()) break;
            const std::string label = line.substr(i + 4, j - i - 4);
            const bool inline_def = line[j] == ':';
            const bool is_def = i == 0 || inline_def;
            if (!label.empty()) {
                if (is_def) fn_defs[label].emplace_back(row, i);
                if (!inline_def && i != 0) fn_refs[label].emplace_back(row, i);
                if (inline_def) fn_refs[label].emplace_back(row, i);
            }
            i = j;
        }

        planning_ok = level > 0 || (planning_ok && IsPlanningLine(line));
    }

    for (const auto &entry : fn_defs) {
        if (entry.second.size() > 1) {
            for (size_t k = 1; k < entry.second.size(); k++) {
                diags.push_back(Diag(entry.second[k].first, entry.second[k].second,
                                     entry.second[k].second + entry.first.size() + 5, OrgLspSeverity::Error,
                                     "duplicate-footnote",
                                     "Footnote `" + entry.first + "` is already defined on line " +
                                         std::to_string(entry.second[0].first + 1) + "."));
            }
        }
        if (fn_refs.find(entry.first) == fn_refs.end()) {
            diags.push_back(Diag(entry.second[0].first, entry.second[0].second,
                                 entry.second[0].second + entry.first.size() + 5, OrgLspSeverity::Hint, "unused-footnote",
                                 "Footnote `" + entry.first + "` is defined but never referenced."));
        }
    }
    for (const auto &entry : fn_refs) {
        if (fn_defs.find(entry.first) != fn_defs.end()) continue;
        for (const auto &pos : entry.second) {
            diags.push_back(Diag(pos.first, pos.second, pos.second + entry.first.size() + 5, OrgLspSeverity::Warning,
                                 "undefined-footnote", "Footnote `" + entry.first + "` is never defined."));
        }
    }

    std::stable_sort(diags.begin(), diags.end(), [](const OrgLspDiagnostic &a, const OrgLspDiagnostic &b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col_start < b.col_start;
    });
    return diags;
}

namespace {

// --- Completion plumbing ----------------------------------------------

// The cursor's own word, as mep's completion client computes it: the
// alnum/underscore run ending at the cursor. Every candidate this server
// returns replaces exactly that run and nothing wider, because
// Editor::AcceptCompletion ignores `textEdit` and splices over precisely
// this span (see org_lsp.h's note on insert_text).
struct CompletionCtx {
    std::string line;
    size_t col = 0;
    size_t word_start = 0;
    std::string word;
    std::string before;  // line[0, word_start)
};

/** @brief Builds the cursor's completion context (its alnum/underscore word and the text before it). */
CompletionCtx MakeCtx(const std::string &line, size_t col) {
    CompletionCtx ctx;
    ctx.line = line;
    ctx.col = std::min(col, line.size());
    size_t start = ctx.col;
    while (start > 0 && IsWordChar(line[start - 1])) start--;
    ctx.word_start = start;
    ctx.word = line.substr(start, ctx.col - start);
    ctx.before = line.substr(0, start);
    return ctx;
}

// Org accepts `#+TITLE:` and `#+title:` alike, so a candidate is offered
// in whichever case the user has already started typing -- and its first
// bytes are then overwritten with the literal typed prefix, so a client
// that filters candidates by exact prefix (mep's does) can never drop its
// own suggestion on a case mismatch.
/** @brief Adjusts a candidate's case to the typed prefix and splices that literal prefix onto its front. */
std::string CaseAdjust(const std::string &canonical, const std::string &word, bool fold) {
    if (!fold || word.empty()) return canonical;
    bool has_lower = false, has_upper = false;
    for (char c : word) {
        if (std::islower(static_cast<unsigned char>(c)) != 0) has_lower = true;
        if (std::isupper(static_cast<unsigned char>(c)) != 0) has_upper = true;
    }
    std::string s = canonical;
    if (has_lower) s = Lower(s);
    else if (has_upper) s = Upper(s);
    if (word.size() <= s.size()) s.replace(0, word.size(), word);
    return s;
}

/** @brief Adds one candidate if it matches the typed word, with `fold` selecting case-insensitive org-keyword matching. */
void Emit(std::vector<OrgLspCompletionItem> *out, const CompletionCtx &ctx, const std::string &canonical, OrgLspKind kind,
          const std::string &detail, const std::string &doc, bool fold) {
    if (canonical.size() < ctx.word.size()) return;
    const bool matches = fold ? IStartsWith(canonical, ctx.word) : canonical.compare(0, ctx.word.size(), ctx.word) == 0;
    if (!matches) return;
    OrgLspCompletionItem item;
    item.insert_text = CaseAdjust(canonical, ctx.word, fold);
    item.label = item.insert_text;
    item.kind = kind;
    item.detail = detail;
    item.documentation = doc;
    item.replace_start = static_cast<int>(ctx.word_start);
    item.replace_end = static_cast<int>(ctx.col);
    out->push_back(item);
}

/** @brief Adds every entry of a vocabulary table as a candidate, optionally with a fixed prefix/suffix around the name. */
void EmitVocab(std::vector<OrgLspCompletionItem> *out, const CompletionCtx &ctx, const std::vector<OrgLspVocabEntry> &vocab,
               OrgLspKind kind, bool fold, const std::string &prefix = "", const std::string &suffix = "") {
    for (const OrgLspVocabEntry &e : vocab) Emit(out, ctx, prefix + e.name + suffix, kind, e.detail, e.doc, fold);
}

/** @brief Adds every string of a set as a candidate with a shared detail annotation. */
void EmitSet(std::vector<OrgLspCompletionItem> *out, const CompletionCtx &ctx, const std::set<std::string> &values,
             OrgLspKind kind, const std::string &detail, const std::string &suffix = "") {
    for (const std::string &v : values) Emit(out, ctx, v + suffix, kind, detail, "", false);
}

// An unclosed `[[` before the cursor: the target being typed, and where
// it starts. `in_description` is true once a `][` has been passed, where
// org expects free text and this server has nothing to offer.
struct LinkCtx {
    bool active = false;
    bool in_description = false;
    size_t target_start = 0;
};

/** @brief Detects whether the cursor sits inside an unclosed `[[...]]` link, and where that link's target begins. */
LinkCtx FindLinkCtx(const std::string &line, size_t col) {
    LinkCtx ctx;
    size_t open = std::string::npos;
    for (size_t i = 0; i + 1 < col; i++) {
        if (line[i] == '[' && line[i + 1] == '[') {
            open = i;
            i++;
        } else if (line[i] == ']' && line[i + 1] == ']') {
            open = std::string::npos;
            i++;
        }
    }
    if (open == std::string::npos) return ctx;
    ctx.active = true;
    ctx.target_start = open + 2;
    const std::string inner = line.substr(ctx.target_start, col - ctx.target_start);
    ctx.in_description = inner.find("][") != std::string::npos;
    return ctx;
}

/** @brief Lists the entries of a directory, appending `/` to subdirectory names, or nothing on any filesystem error. */
std::vector<std::pair<std::string, bool>> ListDir(const std::string &dir) {
    std::vector<std::pair<std::string, bool>> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir), ec);
    if (ec) return out;
    for (const std::filesystem::directory_entry &e : it) {
        std::error_code ec2;
        out.emplace_back(e.path().filename().string(), e.is_directory(ec2));
    }
    return out;
}

/** @brief Offers link targets: schemes, on-disk paths, headline titles, `#+NAME:`s, radio targets and custom ids. */
void CompleteLinkTarget(std::vector<OrgLspCompletionItem> *out, const CompletionCtx &ctx, const LinkCtx &link,
                        const DocIndex &ix, const OrgLspOptions &opts) {
    const std::string typed = ctx.line.substr(link.target_start, ctx.word_start - link.target_start);
    if (!typed.empty() && typed[0] == '*') {
        EmitSet(out, ctx, ix.headline_titles, OrgLspKind::Reference, "headline");
        return;
    }
    if (!typed.empty() && typed[0] == '#') {
        EmitSet(out, ctx, ix.custom_ids, OrgLspKind::Reference, ":CUSTOM_ID:");
        return;
    }
    const bool path_context = typed.find('/') != std::string::npos || IStartsWith(typed, "file:") ||
                              IStartsWith(typed, "./") || IStartsWith(typed, "../");
    if (path_context) {
        if (!opts.check_files) return;
        std::string rel = typed;
        if (IStartsWith(rel, "file:")) rel = rel.substr(5);
        const size_t slash = rel.find_last_of('/');
        const std::string sub = slash == std::string::npos ? std::string() : rel.substr(0, slash + 1);
        const std::string dir = ResolveLinkPath(sub.empty() ? std::string(".") : sub, opts);
        if (dir.empty()) return;
        for (const auto &entry : ListDir(dir)) {
            Emit(out, ctx, entry.first + (entry.second ? "/" : ""), entry.second ? OrgLspKind::Folder : OrgLspKind::File,
                 entry.second ? "directory" : "file", "", false);
        }
        return;
    }
    // A bare target: everything an internal link can name, plus the
    // scheme prefixes that start an external one.
    static const char *const kSchemes[] = {"file:", "https://", "http://", "id:", "mailto:", "attachment:", "elisp:", "shell:"};
    for (const char *scheme : kSchemes) Emit(out, ctx, scheme, OrgLspKind::Module, "link scheme", "", false);
    EmitSet(out, ctx, ix.names, OrgLspKind::Reference, "#+NAME:");
    EmitSet(out, ctx, ix.radio_targets, OrgLspKind::Reference, "<<target>>");
    EmitSet(out, ctx, ix.custom_ids, OrgLspKind::Reference, ":CUSTOM_ID:");
    EmitSet(out, ctx, ix.headline_titles, OrgLspKind::Reference, "headline");
    if (opts.check_files) {
        const std::string dir = ResolveLinkPath(".", opts);
        if (!dir.empty()) {
            for (const auto &entry : ListDir(dir)) {
                Emit(out, ctx, entry.first + (entry.second ? "/" : ""), entry.second ? OrgLspKind::Folder : OrgLspKind::File,
                     entry.second ? "directory" : "file", "", false);
            }
        }
    }
}

}  // namespace

std::vector<OrgLspCompletionItem> OrgLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const OrgLspOptions &opts) {
    std::vector<OrgLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= lines.size() || col < 0) return out;
    const std::string &text = lines[static_cast<size_t>(line)];
    const StructureResult structure = ScanStructure(lines);
    const LineState &st = structure.states[static_cast<size_t>(line)];
    // Inside a src/example/export block the contents are not org at all.
    // mep bridges those to a real language server of their own
    // (kBuiltinOrgPolyglot), so offering org keywords there would be both
    // wrong and in the way.
    if (st.in_literal) return out;

    const CompletionCtx ctx = MakeCtx(text, static_cast<size_t>(col));
    const DocIndex ix = BuildDocIndex(lines, structure.states);

    // 1. Inside `[[...]]`.
    const LinkCtx link = FindLinkCtx(text, ctx.col);
    if (link.active) {
        if (!link.in_description) CompleteLinkTarget(&out, ctx, link, ix, opts);
        return out;
    }

    // 2. Inside `[fn:`.
    {
        const size_t fn = ctx.before.rfind("[fn:");
        if (fn != std::string::npos && ctx.line.find(']', fn) >= ctx.col) {
            std::set<std::string> labels;
            for (size_t i = 0; i < lines.size(); i++) {
                if (structure.states[i].in_literal) continue;
                const std::string &l = lines[i];
                if (l.compare(0, 4, "[fn:") != 0) continue;
                const size_t close = l.find_first_of("]:", 4);
                if (close != std::string::npos && close > 4) labels.insert(l.substr(4, close - 4));
            }
            EmitSet(&out, ctx, labels, OrgLspKind::Reference, "footnote");
            return out;
        }
    }

    // 3. `\entity`.
    if (!ctx.before.empty() && ctx.before.back() == '\\') {
        EmitVocab(&out, ctx, kEntities, OrgLspKind::Constant, false);
        return out;
    }

    // 4. Headline line: TODO keywords, the priority cookie, tags.
    const int level = OrgHeadlineLevel(text);
    if (level > 0) {
        size_t after_stars = static_cast<size_t>(level);
        while (after_stars < text.size() && text[after_stars] == ' ') after_stars++;
        if (ctx.word_start == after_stars) {
            for (const std::string &kw : ix.todo_keywords) Emit(&out, ctx, kw, OrgLspKind::Keyword, "TODO keyword", "", false);
            for (const std::string &kw : ix.done_keywords) Emit(&out, ctx, kw, OrgLspKind::Keyword, "DONE keyword", "", false);
            return out;
        }
        if (ctx.before.size() >= 2 && ctx.before.compare(ctx.before.size() - 2, 2, "[#") == 0) {
            for (const char *p : {"A", "B", "C"}) Emit(&out, ctx, p, OrgLspKind::EnumMember, "priority", "", false);
            return out;
        }
        if (!ctx.before.empty() && ctx.before.back() == ':') {
            EmitSet(&out, ctx, ix.tags, OrgLspKind::EnumMember, "tag", ":");
            return out;
        }
    }

    // 5. Inside a `:PROPERTIES:` drawer.
    if (st.in_drawer && st.drawer_name == "PROPERTIES" && Trim(ctx.before) == ":") {
        EmitVocab(&out, ctx, kProperties, OrgLspKind::Property, false, "", ":");
        return out;
    }

    // 6. A `#+begin_...` opening line: language, header-argument names,
    //    header-argument values.
    const BlockMatch bm = MatchBlock(text);
    if (bm.ok && !bm.is_end && ctx.word_start >= bm.args_start) {
        const bool src = IEq(bm.name, "src");
        if (ctx.word_start == bm.args_start) {
            if (src) EmitVocab(&out, ctx, kBabelLangs, OrgLspKind::Module, false);
            else if (IEq(bm.name, "export")) {
                for (const char *b : {"html", "latex", "ascii", "beamer", "texinfo", "odt", "md"}) {
                    Emit(&out, ctx, b, OrgLspKind::Module, "export backend", "", false);
                }
            }
            return out;
        }
        if (!ctx.before.empty() && ctx.before.back() == ':') {
            EmitVocab(&out, ctx, kHeaderArgs, OrgLspKind::Property, false);
            return out;
        }
        for (const HeaderArg &arg : ParseHeaderArgs(text, bm.args_start)) {
            if (ctx.word_start < arg.value_start || ctx.word_start > arg.value_end) continue;
            const std::vector<const char *> *allowed = HeaderArgValues(arg.key);
            if (allowed == nullptr) return out;
            for (const char *v : *allowed) Emit(&out, ctx, v, OrgLspKind::EnumMember, ":" + arg.key + " value", "", false);
            return out;
        }
        return out;
    }

    // 7. Keyword-line values (`#+OPTIONS:`, `#+STARTUP:`, ...).
    const KeywordMatch km = MatchKeyword(text);
    if (km.ok && ctx.word_start >= km.value_start) {
        const std::string key = Upper(km.key);
        if (key == "OPTIONS") EmitVocab(&out, ctx, kOptions, OrgLspKind::Property, false);
        else if (key == "STARTUP") EmitVocab(&out, ctx, kStartup, OrgLspKind::EnumMember, false);
        else if (key == "FILETAGS" || key == "TAGS") EmitSet(&out, ctx, ix.tags, OrgLspKind::EnumMember, "tag", ":");
        else if (key == "CALL") EmitSet(&out, ctx, ix.names, OrgLspKind::Function, "#+NAME:");
        else if (key == "TODO" || key == "SEQ_TODO" || key == "TYP_TODO") {
            for (const char *kw : {"TODO", "NEXT", "WAITING", "HOLD", "DONE", "CANCELED"}) {
                Emit(&out, ctx, kw, OrgLspKind::Keyword, "workflow keyword", "", false);
            }
        } else if (key == "PROPERTY" || key == "HEADER") {
            if (!ctx.before.empty() && ctx.before.back() == ':') EmitVocab(&out, ctx, kHeaderArgs, OrgLspKind::Property, false);
            else if (key == "PROPERTY") {
                Emit(&out, ctx, "header-args", OrgLspKind::Property, "babel defaults",
                     "Default header arguments for every source block in this file.", false);
                for (const OrgLspVocabEntry &e : kBabelLangs) {
                    Emit(&out, ctx, "header-args:" + std::string(e.name), OrgLspKind::Property, "babel defaults",
                         "Default header arguments for " + std::string(e.name) + " source blocks in this file.", false);
                }
                EmitVocab(&out, ctx, kProperties, OrgLspKind::Property, false);
            }
        } else if (key == "INCLUDE" && opts.check_files) {
            const std::string dir = ResolveLinkPath(".", opts);
            for (const auto &entry : ListDir(dir)) {
                Emit(&out, ctx, entry.first + (entry.second ? "/" : ""), entry.second ? OrgLspKind::Folder : OrgLspKind::File,
                     entry.second ? "directory" : "file", "", false);
            }
        }
        return out;
    }

    // 8. Right after `#+` at the start of a line: every keyword, every
    //    block opener, and -- when a block is still open here -- the
    //    `#+end_` that would close it, offered first.
    const size_t indent = IndentEnd(text);
    if (ctx.word_start == indent + 2 && indent + 1 < text.size() && text[indent] == '#' && text[indent + 1] == '+') {
        if (!st.block_name.empty() && (st.in_block || st.is_block_begin)) {
            Emit(&out, ctx, "end_" + st.block_name, OrgLspKind::Snippet, "closes the open block",
                 "Closes the `#+begin_" + st.block_name + "` this line sits inside.", true);
        }
        EmitVocab(&out, ctx, kKeywords, OrgLspKind::Keyword, true, "", ":");
        EmitVocab(&out, ctx, kBlocks, OrgLspKind::Snippet, true, "begin_");
        EmitVocab(&out, ctx, kBlocks, OrgLspKind::Snippet, true, "end_");
        return out;
    }

    // 9. A planning line, directly under its headline.
    if (line > 0 && OrgHeadlineLevel(lines[static_cast<size_t>(line) - 1]) > 0 && AllBlank(ctx.before)) {
        for (const char *p : {"SCHEDULED", "DEADLINE", "CLOSED"}) {
            Emit(&out, ctx, std::string(p) + ":", OrgLspKind::Keyword, "planning",
                 "Planning information org reads only on the line directly under a headline.", true);
        }
        return out;
    }

    return out;
}

namespace {

// --- Hover ------------------------------------------------------------

/** @brief Looks a name up in a vocabulary table, returning its entry or nullptr. */
const OrgLspVocabEntry *FindVocab(const std::vector<OrgLspVocabEntry> &vocab, const std::string &name) {
    for (const OrgLspVocabEntry &e : vocab) {
        if (IEq(name, e.name)) return &e;
    }
    return nullptr;
}

/** @brief Builds a hover result covering `[col_start, col_end)` on `row`. */
OrgLspHoverInfo Hover(int row, size_t col_start, size_t col_end, std::string text) {
    OrgLspHoverInfo h;
    h.found = true;
    h.line = row;
    h.col_start = static_cast<int>(col_start);
    h.col_end = static_cast<int>(col_end);
    h.text = std::move(text);
    return h;
}

/** @brief Expands the alnum/underscore/hyphen word around a column. */
void WordAround(const std::string &line, size_t col, size_t *start, size_t *end) {
    size_t b = std::min(col, line.size());
    while (b > 0 && (IsWordChar(line[b - 1]) || line[b - 1] == '-')) b--;
    size_t e = std::min(col, line.size());
    while (e < line.size() && (IsWordChar(line[e]) || line[e] == '-')) e++;
    *start = b;
    *end = e;
}

}  // namespace

OrgLspHoverInfo OrgLspHover(const std::vector<std::string> &lines, int line, int col) {
    OrgLspHoverInfo none;
    if (line < 0 || static_cast<size_t>(line) >= lines.size() || col < 0) return none;
    const std::string &text = lines[static_cast<size_t>(line)];
    const size_t c = std::min(static_cast<size_t>(col), text.size());
    const StructureResult structure = ScanStructure(lines);
    const LineState &st = structure.states[static_cast<size_t>(line)];
    if (st.in_literal) return none;
    const std::vector<bool> mask = InlineLiteralMask(text);

    // A timestamp under the cursor: spell the date out, including the
    // weekday it actually falls on (the single most common thing to get
    // wrong when a timestamp is edited by hand).
    for (const TimestampSpan &ts : ScanTimestampSpans(text, mask)) {
        if (c < ts.start || c >= ts.end) continue;
        static const char *const kNames[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        const bool active = text[ts.start] == '<';
        std::string body = ts.body;
        std::string out = active ? "Active timestamp (appears in the agenda)" : "Inactive timestamp (agenda ignores it)";
        if (DigitsAt(body, 0, 4) && body.size() > 9) {
            const int year = std::stoi(body.substr(0, 4));
            const size_t d1 = body.find('-');
            const size_t d2 = body.find('-', d1 + 1);
            if (d1 != std::string::npos && d2 != std::string::npos) {
                const int month = std::atoi(body.substr(d1 + 1, d2 - d1 - 1).c_str());
                const int day = std::atoi(body.substr(d2 + 1, 2).c_str());
                if (month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month)) {
                    out += "\n" + std::string(kNames[WeekdayOf(year, month, day)]) + ", " + body.substr(0, 10);
                }
            }
        }
        return Hover(line, ts.start, ts.end, out);
    }

    // A link under the cursor: what it points at, and how org will
    // resolve it.
    for (size_t i = 0; i + 1 < text.size(); i++) {
        if (mask[i] || text[i] != '[' || text[i + 1] != '[') continue;
        const size_t close = text.find("]]", i + 2);
        if (close == std::string::npos) break;
        if (c >= i && c < close + 2) {
            const std::string body = text.substr(i + 2, close - i - 2);
            const size_t desc = body.find("][");
            const std::string target = Trim(desc == std::string::npos ? body : body.substr(0, desc));
            std::string payload, search;
            const LinkKind kind = ClassifyLink(target, &payload, &search);
            std::string what;
            switch (kind) {
                case LinkKind::External: what = "External link"; break;
                case LinkKind::FilePath: what = "File link"; break;
                case LinkKind::HeadlineSearch: what = "Link to the headline titled `" + payload + "`"; break;
                case LinkKind::CustomId: what = "Link to `:CUSTOM_ID: " + payload + "`"; break;
                case LinkKind::Fuzzy: what = "Internal link to `" + payload + "`"; break;
            }
            std::string out = what + "\n" + target;
            if (!search.empty()) out += "\nsearch: " + search;
            if (desc != std::string::npos) out += "\nshown as: " + body.substr(desc + 2);
            return Hover(line, i, close + 2, out);
        }
        i = close + 1;
    }

    // `\alpha` and friends.
    {
        size_t b = 0, e = 0;
        WordAround(text, c, &b, &e);
        if (b > 0 && text[b - 1] == '\\' && e > b) {
            const OrgLspVocabEntry *entry = FindVocab(kEntities, text.substr(b, e - b));
            if (entry != nullptr) {
                return Hover(line, b - 1, e, "Org entity `\\" + std::string(entry->name) + "` -> " + entry->detail + "\n" +
                                                 entry->doc);
            }
        }
    }

    const int level = OrgHeadlineLevel(text);
    if (level > 0) {
        const OrgOutline outline = ParseOrgOutline(lines);
        for (const OrgHeadline &h : outline.headlines) {
            if (h.line_start != line) continue;
            std::string out = "Headline, level " + std::to_string(h.level);
            if (!h.todo_keyword.empty()) {
                out += "\nState: " + h.todo_keyword + (h.is_done_keyword ? " (done)" : " (not done)");
            }
            if (h.priority != 0) out += "\nPriority: " + std::string(1, h.priority);
            if (!h.tags.empty()) {
                out += "\nTags:";
                for (const std::string &t : h.tags) out += " " + t;
            }
            if (h.scheduled.present) out += "\nSCHEDULED: " + h.scheduled.raw;
            if (h.deadline.present) out += "\nDEADLINE: " + h.deadline.raw;
            out += "\nSubtree: lines " + std::to_string(h.line_start + 1) + "-" + std::to_string(h.line_end + 1);
            return Hover(line, 0, text.size(), out);
        }
        return none;
    }

    // Block markers and their language / header arguments.
    const BlockMatch bm = MatchBlock(text);
    if (bm.ok) {
        if (c >= bm.name_start && c < bm.name_end) {
            const OrgLspVocabEntry *entry = FindVocab(kBlocks, bm.name);
            const std::string head = std::string(bm.is_end ? "Closes" : "Opens") + " a `" + Lower(bm.name) + "` block";
            return Hover(line, bm.name_start, bm.name_end, entry != nullptr ? head + "\n" + entry->doc : head);
        }
        if (!bm.is_end && c >= bm.args_start) {
            size_t lang_end = bm.args_start;
            while (lang_end < text.size() && text[lang_end] != ' ' && text[lang_end] != '\t') lang_end++;
            if (IEq(bm.name, "src") && c < lang_end) {
                const std::string lang = text.substr(bm.args_start, lang_end - bm.args_start);
                const OrgLspVocabEntry *entry = FindVocab(kBabelLangs, lang);
                return Hover(line, bm.args_start, lang_end,
                             entry != nullptr ? "Source language `" + lang + "` -- " + entry->doc
                                              : "Source language `" + lang + "` (mep's babel cannot evaluate it)");
            }
            for (const HeaderArg &arg : ParseHeaderArgs(text, bm.args_start)) {
                if (c < arg.key_start || c >= arg.key_end) continue;
                const OrgLspVocabEntry *entry = FindVocab(kHeaderArgs, arg.key);
                if (entry == nullptr) return Hover(line, arg.key_start, arg.key_end, "Unknown header argument `:" + arg.key + "`");
                return Hover(line, arg.key_start, arg.key_end,
                             "`:" + std::string(entry->name) + "` -- " + entry->detail + "\n" + entry->doc);
            }
        }
        return none;
    }

    // A node property inside a drawer.
    if (st.in_drawer) {
        const size_t p = IndentEnd(text);
        if (p < text.size() && text[p] == ':') {
            const size_t q = text.find(':', p + 1);
            if (q != std::string::npos && c >= p && c <= q) {
                const std::string name = text.substr(p + 1, q - p - 1);
                const OrgLspVocabEntry *entry = FindVocab(kProperties, name);
                if (entry != nullptr) {
                    return Hover(line, p, q + 1, "Property `" + std::string(entry->name) + "` -- " + entry->detail + "\n" +
                                                     entry->doc);
                }
                return Hover(line, p, q + 1, "Node property `" + name + "`");
            }
        }
        return none;
    }

    const KeywordMatch km = MatchKeyword(text);
    if (km.ok) {
        if (c >= km.key_start - 2 && c <= km.key_end) {
            const OrgLspVocabEntry *entry = FindVocab(kKeywords, km.key);
            if (entry != nullptr) {
                return Hover(line, km.key_start - 2, km.key_end + 1,
                             "`#+" + std::string(entry->name) + ":` -- " + entry->detail + "\n" + entry->doc);
            }
            return Hover(line, km.key_start - 2, km.key_end + 1, "`#+" + km.key + ":` is not a keyword org defines.");
        }
        if (IEq(km.key, "OPTIONS")) {
            size_t b = std::min(c, text.size()), e = b;
            while (b > km.value_start && text[b - 1] != ' ' && text[b - 1] != '\t') b--;
            while (e < text.size() && text[e] != ' ' && text[e] != '\t') e++;
            const std::string tok = text.substr(b, e - b);
            const size_t colon = tok.find(':');
            if (colon != std::string::npos) {
                const OrgLspVocabEntry *entry = FindVocab(kOptions, tok.substr(0, colon + 1));
                if (entry != nullptr) {
                    return Hover(line, b, e, "`" + std::string(entry->name) + "` (" + entry->detail + ")\n" + entry->doc);
                }
            }
        }
        if (IEq(km.key, "STARTUP")) {
            size_t b = std::min(c, text.size()), e = b;
            while (b > km.value_start && text[b - 1] != ' ' && text[b - 1] != '\t') b--;
            while (e < text.size() && text[e] != ' ' && text[e] != '\t') e++;
            const OrgLspVocabEntry *entry = FindVocab(kStartup, text.substr(b, e - b));
            if (entry != nullptr) {
                return Hover(line, b, e, "`#+STARTUP: " + std::string(entry->name) + "` (" + entry->detail + ")\n" + entry->doc);
            }
        }
        if (IEq(km.key, "PROPERTY") || IEq(km.key, "HEADER")) {
            for (const HeaderArg &arg : ParseHeaderArgs(text, km.value_start)) {
                if (c < arg.key_start || c >= arg.key_end) continue;
                const OrgLspVocabEntry *entry = FindVocab(kHeaderArgs, arg.key);
                if (entry != nullptr) {
                    return Hover(line, arg.key_start, arg.key_end,
                                 "`:" + std::string(entry->name) + "` -- " + entry->detail + "\n" + entry->doc);
                }
            }
        }
    }
    return none;
}

// --- Document symbols -------------------------------------------------

std::vector<OrgLspSymbol> OrgLspSymbols(const std::vector<std::string> &lines) {
    std::vector<OrgLspSymbol> out;
    const OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        OrgLspSymbol sym;
        sym.name = h.title.empty() ? std::string("(untitled)") : h.title;
        if (!h.todo_keyword.empty()) sym.detail = h.todo_keyword;
        if (h.priority != 0) sym.detail += (sym.detail.empty() ? "" : " ") + std::string("[#") + h.priority + "]";
        for (const std::string &t : h.tags) sym.detail += (sym.detail.empty() ? ":" : " :") + t + ":";
        sym.line_start = h.line_start;
        sym.line_end = h.line_end;
        // The title's own columns, for `selectionRange`: everything after
        // the stars, the keyword and the priority cookie.
        const std::string &text = lines[static_cast<size_t>(h.line_start)];
        const size_t at = text.find(h.title);
        sym.sel_col_start = static_cast<int>(at == std::string::npos ? 0 : at);
        sym.sel_col_end = sym.sel_col_start + static_cast<int>(h.title.size());
        sym.parent = h.parent_index;
        out.push_back(sym);
    }
    return out;
}

// --- Folding ----------------------------------------------------------

std::vector<OrgLspFold> OrgLspFolds(const std::vector<std::string> &lines) {
    std::vector<OrgLspFold> out;
    const OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        if (h.line_end > h.line_start) out.push_back({h.line_start, h.line_end, ""});
    }
    const StructureResult structure = ScanStructure(lines);
    std::vector<int> block_stack, drawer_stack;
    for (size_t i = 0; i < lines.size(); i++) {
        const LineState &st = structure.states[i];
        const int row = static_cast<int>(i);
        if (st.is_block_begin) block_stack.push_back(row);
        if (st.is_block_end && !block_stack.empty()) {
            const int start = block_stack.back();
            block_stack.pop_back();
            if (row > start) out.push_back({start, row, st.block_name == "comment" ? "comment" : ""});
        }
        if (st.is_drawer_begin) drawer_stack.push_back(row);
        if (st.is_drawer_end && !drawer_stack.empty()) {
            const int start = drawer_stack.back();
            drawer_stack.pop_back();
            if (row > start) out.push_back({start, row, ""});
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const OrgLspFold &a, const OrgLspFold &b) { return a.start_line < b.start_line; });
    return out;
}

// --- Definition -------------------------------------------------------

OrgLspLocation OrgLspDefinition(const std::vector<std::string> &lines, int line, int col, const OrgLspOptions &opts) {
    OrgLspLocation loc;
    if (line < 0 || static_cast<size_t>(line) >= lines.size() || col < 0) return loc;
    const std::string &text = lines[static_cast<size_t>(line)];
    const size_t c = std::min(static_cast<size_t>(col), text.size());
    const std::vector<bool> mask = InlineLiteralMask(text);
    std::string target;
    for (size_t i = 0; i + 1 < text.size(); i++) {
        if (mask[i] || text[i] != '[' || text[i + 1] != '[') continue;
        const size_t close = text.find("]]", i + 2);
        if (close == std::string::npos) break;
        if (c >= i && c < close + 2) {
            const std::string body = text.substr(i + 2, close - i - 2);
            const size_t desc = body.find("][");
            target = Trim(desc == std::string::npos ? body : body.substr(0, desc));
            break;
        }
        i = close + 1;
    }
    if (target.empty()) return loc;

    std::string payload, search;
    const LinkKind kind = ClassifyLink(target, &payload, &search);
    if (kind == LinkKind::External) return loc;
    if (kind == LinkKind::FilePath) {
        const std::string resolved = ResolveLinkPath(payload, opts);
        if (resolved.empty() || (opts.check_files && !PathExists(resolved))) return loc;
        loc.found = true;
        loc.path = resolved;
        // `file.org::42` jumps to that line; `::*Headline` / `::text`
        // would need the target file parsed, which this deliberately does
        // not do (the file is about to be opened anyway).
        if (!search.empty() && search.find_first_not_of("0123456789") == std::string::npos) {
            loc.line = std::max(0, std::atoi(search.c_str()) - 1);
        }
        return loc;
    }

    const StructureResult structure = ScanStructure(lines);
    if (kind == LinkKind::CustomId) {
        for (size_t i = 0; i < lines.size(); i++) {
            if (!structure.states[i].in_drawer) continue;
            const std::string &l = lines[i];
            const size_t p = IndentEnd(l);
            if (p >= l.size() || l[p] != ':') continue;
            const size_t q = l.find(':', p + 1);
            if (q == std::string::npos || !IEq(l.substr(p + 1, q - p - 1), "CUSTOM_ID")) continue;
            if (Trim(l.substr(q + 1)) != payload) continue;
            loc.found = true;
            loc.line = structure.states[i].headline_line >= 0 ? structure.states[i].headline_line : static_cast<int>(i);
            return loc;
        }
        return loc;
    }
    // A headline search or a fuzzy target: headlines first (org's own
    // precedence), then `#+NAME:`, then `<<radio targets>>`.
    const OrgOutline outline = ParseOrgOutline(lines);
    for (const OrgHeadline &h : outline.headlines) {
        if (Trim(h.title) != payload) continue;
        loc.found = true;
        loc.line = h.line_start;
        return loc;
    }
    if (kind == LinkKind::HeadlineSearch) return loc;
    for (size_t i = 0; i < lines.size(); i++) {
        if (structure.states[i].in_literal) continue;
        const KeywordMatch km = MatchKeyword(lines[i]);
        if (km.ok && IEq(km.key, "NAME") && km.value == payload) {
            loc.found = true;
            loc.line = static_cast<int>(i);
            return loc;
        }
    }
    for (size_t i = 0; i < lines.size(); i++) {
        if (structure.states[i].in_literal) continue;
        const std::string &l = lines[i];
        const std::vector<bool> m = InlineLiteralMask(l);
        for (size_t j = 0; j + 1 < l.size(); j++) {
            if (m[j]) continue;
            std::string label;
            size_t end = 0;
            if (RadioTargetAt(l, j, &label, &end) && label == payload) {
                loc.found = true;
                loc.line = static_cast<int>(i);
                loc.col = static_cast<int>(j);
                return loc;
            }
        }
    }
    return loc;
}
