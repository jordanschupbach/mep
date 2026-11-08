#pragma once

// The names mep-tcl-lsp knows: Tcl's built-in commands and their
// subcommands (with Tcl's own usage lines and the argument counts tclsh
// accepts), expr's functions and Tcl's variables, documented from Tcl's
// manual pages. Generated into tcl_lsp_vocab.cpp by tools/gen_tcl_vocab.py.

#include <cstddef>

struct TclLspApi {
    const char *name;   // "set", "string length", "abs" (a function), "env" (a variable)
    const char *kind;   // "command", "ensemble", "subcommand", "function", "variable"
    const char *usage;  // "set varName ?newValue?"; a function's: "abs arg"
    unsigned mask;      // bit n: n arguments (after the command or subcommand) are accepted; 0 = unknown
    bool more;          // more than 30 arguments are accepted
    const char *doc;    // markdown
};

const TclLspApi *TclLspApiTable(size_t *count);
