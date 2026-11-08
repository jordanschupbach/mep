#pragma once

// The names mep-julia-lsp knows without reading any package: Core's and
// Base's exported and public names and those of the standard library
// modules, with their kind, the signatures their docstrings open with, and
// the docstrings (Julia is MIT-licensed). Generated into julia_lsp_vocab.cpp
// by tools/gen_julia_vocab.jl (`just julia-vocab`).

#include <cstddef>

struct JuliaLspApi {
    const char *module;     // "Core", "Base", "LinearAlgebra", ...
    const char *name;       // "sum", "@time", "AbstractArray"
    const char *kind;       // "function", "macro", "type", "module", "constant"
    const char *signature;  // "sum(f, itr; [init])\nsum(itr; [init])" -- "" when the docs give none
    const char *doc;        // markdown
};

const JuliaLspApi *JuliaLspApiTable(size_t *count);
