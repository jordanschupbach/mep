#ifndef MEP_CLOJURE_LSP_CORE_NAMES_H
#define MEP_CLOJURE_LSP_CORE_NAMES_H

#include <cstddef>

// The vocabulary of mep's own Clojure language server: the vars of
// clojure.core and the namespaces that ship with Clojure, and the special
// forms -- generated from Clojure itself by tools/gen_clojure_vocab.clj
// into clojure_lsp_core_names.cpp.
struct CljCoreName {
    const char *ns;          // "clojure.core", ...; "" for a special form
    const char *name;
    const char *kind;        // "special", "macro", "function", "dynamic", "var"
    const char *arglists;    // one per line, as written: "[x]\n[x & more]"
    const char *doc;         // the docstring, its continuation lines unindented
    const char *added;       // "1.0", or ""
    const char *deprecated;  // the version it was deprecated in, or ""
    bool is_private;
};

/** @brief The whole table and its size. */
const CljCoreName *CljCoreNames(size_t *count);

#endif  // MEP_CLOJURE_LSP_CORE_NAMES_H
