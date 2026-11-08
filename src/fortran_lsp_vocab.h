#ifndef MEP_FORTRAN_LSP_VOCAB_H
#define MEP_FORTRAN_LSP_VOCAB_H

// The names mep's Fortran server knows without reading any source: every
// intrinsic procedure gfortran documents (standard and GNU extensions, with
// the specific names such as DABS), the public entities of the intrinsic
// modules (ISO_FORTRAN_ENV, ISO_C_BINDING, IEEE_ARITHMETIC, IEEE_EXCEPTIONS,
// IEEE_FEATURES, OMP_LIB, OMP_LIB_KINDS), the modules themselves and the
// statement keywords. Generated into fortran_lsp_vocab.cpp by
// tools/gen_fortran_vocab.py -- names, signatures, class and standard from the
// gfortran manual and gfortran's module files; the summaries are mep's own.
//
// Notes on the data:
//  - signature lines are upper case as the manual writes them, one form per
//    line, optional arguments in [ ] (each one bracketed on its own:
//    "SUM(ARRAY, DIM [, MASK])"), a trailing "[, ...]" for MAX/MIN's open
//    list. A procedure gfortran offers as both subroutine and function
//    (klass "subroutine, function", e.g. CHDIR) keeps "CALL " in front of its
//    subroutine form; its kind is Function.
//  - min_args/max_args span every form; -1/-1 for non-procedures.
//  - the ISO_C_BINDING procedures (c_loc, c_f_pointer, ...) and
//    COMPILER_VERSION/COMPILER_OPTIONS carry their module.
//  - a few keyword names are two words ("select case", "error stop"), and a
//    keyword that is also an intrinsic's name (real, exit, flush, len, ...)
//    has its own Keyword entry right after the intrinsic's, so
//    FortranVocabFind returns the procedure; scan forward for the rest.

#include <cstddef>

enum class FortranVocabKind { Function, Subroutine, Constant, Type, Module, Keyword };

struct FortranVocabEntry {
    const char *name;          // lowercase
    FortranVocabKind kind;
    const char *module;        // "" for intrinsic procedures and keywords; else e.g. "iso_c_binding"
    const char *signature;     // e.g. "SUM(ARRAY [, MASK])\nSUM(ARRAY, DIM [, MASK])"; "" for constants/keywords
    const char *klass;         // "elemental function", "inquiry function", "subroutine", ...; "" if n/a
    const char *standard;      // "Fortran 95", "Fortran 2008", "GNU extension", ...
    const char *summary;       // one line, our own words
    short min_args, max_args;  // positional; max_args -1 = unbounded (MAX/MIN); both -1 when unknown
    const char *generic;       // for a specific name (dabs) its generic ("abs"); else ""
};

const FortranVocabEntry *FortranVocab(size_t *count);                          // sorted by (name, module)
const FortranVocabEntry *FortranVocabFind(const char *lower_name, size_t len);  // first match by name, binary search

#endif
