#pragma once

// Packages a document `using`s, found on the Julia load path and analysed
// with mep's own parser (julia_lsp_packages.cpp): their exported names,
// docstrings, signatures and definition sites.

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "julia_lsp_vocab.h"

namespace jllsp {

struct PackageRow {
    JuliaLspApi api;            // module = the package
    bool exported = false;      // visible after `using Package`
    bool extends_base = false;  // has methods on a Base function (`Base.size(x::T)`)
    bool has_location = false;
    int line = 0, col = 0;      // the first definition, in Package::file
};

struct Package {
    std::string name, file;
    std::string doc;  // the module's docstring
    bool complete = true;  // no include/eval/@reexport: the export list is all there is
    std::vector<std::string> exports;
    std::vector<PackageRow> rows;
    std::multimap<std::string, size_t> by_name;
    std::vector<std::unique_ptr<std::string>> strings;  // what rows point into
};

/** @brief Package @p name as `using` from file @p from would load it (nullptr: not found). Cached. */
std::shared_ptr<const Package> FindPackage(const std::string &name, const std::string &from);
/** @brief @p p's rows named @p name. */
std::vector<const PackageRow *> PackageRows(const Package &p, std::string_view name);

}  // namespace jllsp
