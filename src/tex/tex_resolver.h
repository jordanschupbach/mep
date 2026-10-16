#ifndef MEP_TEX_RESOLVER_H
#define MEP_TEX_RESOLVER_H

#include <string>
#include <unordered_map>
#include <vector>

namespace meptex {

// Where the engine finds the files it opens by name: \input files, font
// metrics, formats. The role kpathsea plays for TeX Live, kept to what the
// engine needs: a list of directories searched in order (the document's
// own, then any the caller adds), then trees indexed once, the way a
// TeX tree's ls-R database is (plans/TEX_ENGINE_PLAN.md, phase 5 -- the
// embedded bundle and the system texmf tree are both trees here).

enum ResolverKind {
    kTexFile = 1,  // \input, \openin: tried with `.tex` appended first, then as given
    kTfmFile = 2,  // font metrics (TeX has already appended `.tfm`)
    kFmtFile = 3,  // formats
    kOtherFile = 4,  // images, font files, maps, encodings: as given, from fonts/ or tex/
};

class Resolver {
public:
    /** @brief Adds a directory searched (not recursively) before any tree. */
    void add_dir(const std::string &dir);
    /**
     * @brief Indexes a TeX tree (a texmf root). Its files are found by name
     * from the subtrees that hold each kind: tex/ (LaTeX, then generic,
     * then plain, then the rest), fonts/tfm/, web2c/ for formats.
     */
    void add_tree(const std::string &root);

    /**
     * @brief The path of the file TeX means by `name` as a file of `kind`, or "".
     * A name found in "." is returned as "./name", as kpathsea returns it, so
     * the log shows it the way TeX Live's does.
     */
    std::string find(const std::string &name, int kind) const;

private:
    std::vector<std::string> dirs_;
    // basename -> paths, in priority order, one table per kind
    std::unordered_map<std::string, std::vector<std::string>> tex_, tfm_, fmt_, other_;
};

}  // namespace meptex

#endif
