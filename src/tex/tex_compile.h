#ifndef MEP_TEX_COMPILE_H
#define MEP_TEX_COMPILE_H

// `mep-tex compile`: a document typeset to PDF by mep's own engine, the
// way `pdflatex` (or latexmk) does it for TeX Live -- the format built
// once and cached, the passes repeated until cross-references settle.
//
// The TeX files (LaTeX, packages, fonts) come from TeX trees: the ones
// given, else mep's own bundle (tex/tex_bundle.h) and/or an installed TeX
// Live's ($MEP_TEX_TREES, kpsewhich, or the usual places), as `packages`
// says. Formats are kept under $XDG_CACHE_HOME/mep/tex/formats
// (~/.cache/...), one directory per engine build, format and set of
// trees, so a new mep-tex or an updated TeX Live gets a fresh one.

#include <string>
#include <vector>

namespace meptex {

struct CompileOptions {
    std::string tex;                  // the document
    std::string outdir;               // where the PDF, log and aux files go ("" = the document's directory)
    std::string format;               // pdflatex, pdftex, or "" (from the document: %&name, \documentclass)
    std::vector<std::string> trees;   // TeX trees (empty = from `packages`)
    std::string packages;             // bundle, bundle+system, system ("" = $MEP_TEX_PACKAGES, else bundle)
    std::vector<std::string> dirs;    // more directories searched for the document's own files (after its own)
    std::string cache_dir;            // "" = $XDG_CACHE_HOME/mep/tex
    int max_passes = 4;               // runs, at most, while labels and the like change
    bool rebuild_format = false;      // build the format even if a cached one exists
    bool verbose = false;             // say on stderr what is being done
};

/** @brief TeX trees to read from when none are given: $MEP_TEX_TREES, else TeX Live's. */
std::vector<std::string> DiscoverTexTrees();

/**
 * @brief The TeX trees for a package source: `bundle` (mep's own tree, or
 * the system's when no bundle is installed), `bundle+system` (the system's
 * trees, then the bundle), or `system`. Empty with `*err` set if none.
 */
std::vector<std::string> TexTreesFor(const std::string &packages, const std::string &cache_dir, bool verbose,
                                     std::string *err);

/** @brief The default cache directory: $XDG_CACHE_HOME/mep/tex, else ~/.cache/mep/tex. */
std::string DefaultTexCacheDir();

/** @brief The format a document wants: `%&name` on its first line, else pdflatex for LaTeX, else pdftex. */
std::string FormatForDocument(const std::string &text);

/**
 * @brief Builds (or finds in the cache) `format`'s .fmt for `trees` and
 * returns the directory holding it; "" with `*err` set if it cannot.
 */
std::string EnsureFormat(const std::string &format, const std::vector<std::string> &trees,
                         const std::string &cache_dir, bool rebuild, bool verbose, std::string *err);

/**
 * @brief Typesets opt.tex. The engine's terminal output goes to stdout (an
 * error shows there as TeX's `! ...` line). Returns 0 when the PDF was
 * written without errors, 1 otherwise (with `*err` set when the failure
 * was before TeX ran).
 */
int CompileDocument(const CompileOptions &opt, std::string *err);

}  // namespace meptex

#endif
