#ifndef MEP_MAXIMA_LSP_BUILTIN_NAMES_H
#define MEP_MAXIMA_LSP_BUILTIN_NAMES_H

#include <cstddef>

// The interface to src/maxima_lsp_builtin_names.cpp, which is generated
// by scripts/gen_maxima_names.py out of a real Maxima installation's own
// documentation index. See that script's header for why it is generated
// rather than written: a server that remembers a computer-algebra
// system's 2,000-name vocabulary gets it wrong, and gets it wrong in the
// direction that invents diagnostics.

// One documented name, as the generator found it.
struct MaximaBuiltinDecl {
    const char *name;       // "integrate"
    const char *kind;       // "function", "variable", "constant", "operator", "symbol", "property"
    const char *signature;  // "integrate (expr, x, a, b)", or "" when it takes no arguments
    const char *package;    // the share package that carries it, or "" for the core language
};

// Every documented name, sorted by byte comparison.
extern const char *const kMaximaBuiltinNames[];
extern const size_t kMaximaBuiltinNameCount;

// The same names with their kind and argument list, in the same order.
extern const MaximaBuiltinDecl kMaximaBuiltinDecls[];
extern const size_t kMaximaBuiltinDeclCount;

#endif  // MEP_MAXIMA_LSP_BUILTIN_NAMES_H
