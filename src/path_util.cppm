module;

#include <cctype>
#include <string>

export module mep.path_util;

// Shared by doc_export.cpp/office_doc.cpp/sheet_doc.cpp, which each used to
// carry their own identical copy of this -- harmless until
// BUILD_PERFORMANCE_PLAN.md Round 2 Phase C's unity build merges several
// .cpp files into one translation unit, where duplicate same-named
// anonymous-namespace helpers collide (`-Werror=unused-function` on
// whichever copy loses name lookup). Converted from a plain `inline`
// header into a real C++20 module in Phase D, alongside `mep.diff` --
// same shape (small, macro-free, standard-library-only leaf), demonstrating
// the pattern generalizes rather than being a one-off. Kept at global
// scope (not put in a namespace the original header didn't have) so every
// existing unqualified `LowerExt(...)` call site stays unchanged.

/**
 * @brief Returns a path's file extension, lowercased and without the leading dot.
 * @param path File path to extract the extension from.
 * @return The lowercased extension, or an empty string if `path` has no '.'.
 */
export inline std::string LowerExt(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}
