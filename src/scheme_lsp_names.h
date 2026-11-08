#ifndef MEP_SCHEME_LSP_NAMES_H
#define MEP_SCHEME_LSP_NAMES_H

// mep-scheme-lsp's vocabulary: every name the modules Guile ships export
// -- (guile), R7RS-small's (scheme ...) libraries, the SRFIs, the common
// (ice-9 ...) and (rnrs ...) modules -- with its kind, the argument counts
// Guile's own arity allows, and its argument list. Generated from a
// running Guile by tools/gen_scheme_vocab.scm into scheme_lsp_names.cpp;
// sorted by name, then module.

enum class SchemeVocabKind { Syntax, Procedure, Variable };

struct SchemeVocabEntry {
    const char *name;
    const char *module;  // "guile", "scheme base", "srfi srfi-1", ...
    SchemeVocabKind kind;
    int min_args;  // -2: unknown (keyword arguments, or not a procedure)
    int max_args;  // -1: any number more; -2: unknown
    const char *signature;  // "(list-tail list k)", "" when none
};

/** @brief The first entry of the generated table. */
const SchemeVocabEntry *SchemeVocabBegin();
/** @brief One past the last entry of the generated table. */
const SchemeVocabEntry *SchemeVocabEnd();

#endif  // MEP_SCHEME_LSP_NAMES_H
