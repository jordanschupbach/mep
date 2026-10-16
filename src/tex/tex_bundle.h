#ifndef MEP_TEX_BUNDLE_H
#define MEP_TEX_BUNDLE_H

// mep's own TeX tree: the part of TeX Live that LaTeX documents need
// (the LaTeX kernel, amsmath, graphics, xcolor, hyperref, geometry,
// beamer, tcolorbox/TikZ, standalone, Latin Modern and the CM/AMS fonts,
// English hyphenation), packed into one file by tools/tex_bundle.py and
// installed beside mep as share/mep/tex/tex-bundle.mtb. With it, mep-tex
// typesets without a TeX installation.
//
// The engine reads files by path, so the bundle is unpacked once into
// the cache (~/.cache/mep/tex/bundle-<hash>/, with an ls-R the resolver
// reads) and used as an ordinary TeX tree from then on.

#include <string>

namespace meptex {

/**
 * @brief The bundle file: $MEP_TEX_BUNDLE, else ../share/mep/tex/ beside
 * the running executable, else beside it (a build directory); "" if none.
 */
std::string FindTexBundle();

/**
 * @brief The bundle unpacked under `cache_dir` (unpacking it the first
 * time, saying so on stderr when `verbose`): the tree's root, or "" with
 * `*err` set.
 */
std::string UnpackTexBundle(const std::string &bundle, const std::string &cache_dir, bool verbose, std::string *err);

}  // namespace meptex

#endif
