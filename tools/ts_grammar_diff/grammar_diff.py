#!/usr/bin/env python3
"""Compare a tree-sitter grammar of mep's own against the one it replaces.

The method in plans/TREESITTER_GRAMMAR_PLAN.md, as commands:

  corpus  <lang>   fetch and unpack a body of real code in the language
                   (from nixpkgs sources) and write its file list
  symbols <name>   the old and new grammars' node types and fields, and
                   which ones the new grammar lacks
  diff    <name>   parse the corpus with both grammars and compare every
                   named node: type, field and byte range; --oracle asks
                   the language's own implementation about every file
                   whose trees differ
  fuzz    <name>   parse damaged copies of the corpus with the new grammar
                   under AddressSanitizer + UBSan
  recovery <name>  the same damaged copies with both grammars: how many
                   bytes each leaves inside ERROR nodes

<name> is a grammar: lua, c, cpp, python, javascript, typescript, tsx, fish,
markdown, markdown_inline, org, r, nix, just, mepml. The "new" grammar is
grammars/tree-sitter-<name>/src; the "old" is third_party/grammars/<name>/src
while it is still vendored, and that directory as of the last commit that
had it once it is gone (so the oracle outlives the vendored copy), or, for
a grammar mep loaded from nixpkgs instead (typescript, tsx, fish), the nixpkgs
source named by its 'old' entry. Either can be overridden with
--old/--new <src dir>.

Builds and corpora go under build/grammar-diff/ and build/grammar-corpus/.
"""

import argparse
import concurrent.futures
import collections
import fnmatch
import hashlib
import io
import os
import random
import re
import shutil
import subprocess
import sys
import tarfile
import textwrap
import threading
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL_DIR = Path(__file__).resolve().parent
BUILD = REPO / 'build' / 'grammar-diff'
CORPUS = REPO / 'build' / 'grammar-corpus'
RUNTIME = REPO / 'third_party' / 'tree-sitter'

# Characters every language's scanner should survive being cut at or fed.
GENERIC_TOKENS = ['"', "'", '`', '(', ')', '[', ']', '{', '}', '\n', '\\', '#', '/*', '*/', '${', '"""', "''"]

# Per language: the files to collect, where to get a corpus (nixpkgs
# attributes, each with the member paths to keep), the language's own
# "does this parse?" check ({} is the file), and extra fuzz tokens.
# Grammars that share a language (markdown, markdown_inline) share an entry.
# The registry packages in Julia's corpus ('@julia-packages'), with their
# dependencies: data, statistics, AD, optimisation, ML, ODEs, docs, CAS.
JULIA_PACKAGES = ['DataFrames', 'StaticArrays', 'ForwardDiff', 'Distributions', 'Documenter', 'JuliaFormatter',
                  'Runic', 'Symbolics', 'JuMP', 'Flux', 'DifferentialEquations']

# Crates from rustc's vendor/ directory in the Rust corpus (one version each).
VENDORED_RUST_CRATES = [
    'tokio-1.52.1', 'serde-1.0.228', 'serde_json-1.0.149', 'syn-2.0.117', 'regex-1.12.3',
    'regex-automata-0.4.14', 'regex-syntax-0.8.10', 'clap_builder-4.6.0', 'hashbrown-0.17.1', 'rayon-1.12.0',
    'rayon-core-1.13.0', 'crossbeam-channel-0.5.15', 'crossbeam-epoch-0.9.18', 'futures-util-0.3.32',
    'itertools-0.14.0', 'indexmap-2.14.0', 'toml_edit-0.25.11+spec-1.1.0', 'winnow-1.0.2', 'gimli-0.33.0',
    'object-0.39.1', 'memchr-2.8.0', 'aho-corasick-1.1.4', 'anyhow-1.0.102', 'thiserror-2.0.18',
    'proc-macro2-1.0.106', 'quote-1.0.45', 'rowan-0.15.18', 'ena-0.14.3', 'smallvec-1.15.1', 'bstr-1.12.1',
    'ignore-0.4.25', 'globset-0.4.18', 'tracing-0.1.44', 'tracing-subscriber-0.3.23', 'salsa-0.26.2',
    'petgraph-0.8.2', 'nom-7.1.3', 'parking_lot-0.12.5',
]

LANGUAGES = {
    'r': dict(
        globs=['*.R', '*.r'],
        sources=[('R.src', ['*.R', '*.r'])] + [
            (f'rPackages.{p}.src', ['*.R', '*.r'])
            for p in ['ggplot2', 'dplyr', 'data_table', 'rlang', 'shiny', 'testthat', 'vctrs', 'purrr', 'knitr',
                      'Matrix', 'R6']
        ],
        oracle=['Rscript', '-e', 'invisible(parse(commandArgs(TRUE)[1]))', '{}'],
        tokens=['r"(', 'r"--[', ')"', '%', 'else ', 'function(', '[[', ']]', '<-', '\\('],
    ),
    'nix': dict(
        globs=['*.nix'],
        sources=[('@nixpkgs', ['*.nix'])],
        oracle=['nix-instantiate', '--parse', '{}'],
        tokens=["''", "''$", "'''", '${', './', '~/', '<', '//', ':'],
    ),
    'just': dict(
        globs=['justfile', 'Justfile', '.justfile', '*.just'],
        sources=[('just.src', ['*/justfile', '*.just']), ('@just-tests', [])],
        oracle=['just', '--justfile', '{}', '--dump'],
        tokens=['{{', '}}', '{{{{', ':=', '\n    ', '\n', '@', '-', '\\\n'],
    ),
    'mepml': dict(globs=['*.mepml'], sources=[], oracle=None, tokens=['*', '_', '```', '$$', '|']),
    # Maxima's own reader is the oracle for trees as well as validity:
    # tools/ts_grammar_diff/maxima_check.py compares every tree with the one
    # mep's Maxima parser (src/maxima_ast.cpp, which agrees with Maxima's
    # mread-raw on all of share/) builds. The grammar mep loaded before
    # (stewsat/tree-sitter-maxima) gets Maxima's precedences wrong, so it is
    # no oracle for shapes.
    'maxima': dict(
        globs=['*.mac'],
        sources=[('maxima.src', ['*.mac'])],
        oracle=None,
        tokens=['/*', '*/', ':=', '$', ';', 'then', 'do', '(', ')', '[', ']', '\\'],
    ),
    # Guile's own modules and test suite, Guix (Guile, every package
    # definition), haunt and guile-lib, chibi-scheme (R7RS libraries,
    # .sld), Chez Scheme (R6RS, .ss/.sls) and Gauche. The old grammar is
    # nixpkgs' tree-sitter-scheme (6cdh). The oracle is Guile's `read`
    # (scheme_read.scm): R7RS + R6RS lexical syntax, nothing evaluated.
    'scheme': dict(
        globs=['*.scm', '*.ss', '*.sld', '*.sls', '*.sps'],
        sources=[('guile.src', ['*/module/*.scm', '*/test-suite/*.scm', '*/test-suite/*.test']),
                 ('guix.src', ['*.scm']),
                 ('haunt.src', ['*.scm']),
                 ('guile-lib.src', ['*.scm']),
                 ('chibi.src', ['*.scm', '*.sld']),
                 ('chez.src', ['*.ss', '*.sls', '*.sps']),
                 ('gauche.src', ['*.scm'])],
        dedupe=True,
        old=('tree-sitter-grammars.tree-sitter-scheme.src', 'src'),
        oracle=['guile', '--no-auto-compile', '-s', str(Path(__file__).resolve().parent / 'scheme_read.scm'), '{}'],
        tokens=['(', ')', '[', ']', '"', '\\', '#|', '|#', '#;', '#\\', '#(', '#u8(', '#vu8(', "'", '`', ',', ',@',
                '#\'', '#`', '#,', '#,@', '|', ';', '#:', '#!', '#t', '#f', '1/2', '.', '+i', '\n'],
    ),
    # Tcl's and Tk's own libraries and test suites, and tcllib. The old
    # grammar is nixpkgs' tree-sitter-tcl (dropped from nixpkgs-unstable
    # since; flake.lock's still has it). The oracle is Tcl's own parser:
    # tcl_check.tcl compiles the file without running it.
    # Go: the Go 1.26 tree's own src/ (the toolchain, the standard library,
    # their tests and testdata -- some deliberately invalid), then Go
    # programs nixpkgs packages (their vendored modules included). The old
    # grammar is tree-sitter-go 0.25.0 from flake.lock's nixpkgs. The oracle
    # is go/parser, the parser behind gofmt (go_check.go; `just go-check`).
    'go': dict(
        globs=['*.go'],
        sources=[('@pinned:go', ['share/go/src/*.go'])] + [
            (f'{p}.src', ['*.go']) for p in ['gh', 'hugo', 'fzf', 'delve', 'caddy', 'go-tools']],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-go.src', 'src'),
        oracle=[str(REPO / 'build' / 'grammar-diff' / 'go' / 'go-check'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '`', '"', "'", '/*', '*/', '//', ';', '\n', ':=', '<-', '...', '.',
                'func ', 'type ', 'interface{', 'struct{', 'chan ', 'go ', 'case ', 'default:', '[T any]', '0x', '1e'],
    ),
    # Kotlin: the 2.4.10 standard library and compiler (their Maven sources
    # jars: the stdlib, the K1/K2 front ends, IR, backends), then
    # kotlinx.coroutines, kotlinx.serialization, ktor and okhttp (with their
    # Gradle .kts build scripts) and ktfmt. The old grammar is nixpkgs'
    # tree-sitter-kotlin (fwcd, 0.3.8). The oracle is the compiler's own PSI
    # parser (KotlinCheck.java; `just kotlin-check`).
    'kotlin': dict(
        globs=['*.kt', '*.kts'],
        sources=[('@maven:org.jetbrains.kotlin/kotlin-stdlib/2.4.10/sources/'
                  'sha256-XE+oTslFpjRT5kvAFBwiLs6AaiAmwpYf52sL5IOJYyM=', ['*.kt', '*.kts']),
                 ('@maven:org.jetbrains.kotlin/kotlin-compiler/2.4.10/sources/'
                  'sha256-emgxPg3t/naGXW4IG71+lbw78uCZtWE0VfHnpoafvCc=', ['*.kt', '*.kts']),
                 ('@github:Kotlin/kotlinx.coroutines/1.11.0/sha256-7BZkE5t5K4kGPbyJwVz86SJ1k7qJXlSd6UQt2PSSKVA=',
                  ['*.kt', '*.kts']),
                 ('@github:Kotlin/kotlinx.serialization/v1.12.0/sha256-5oYw+MnwNvOy+dNRDq2uxSKopngSCKlhtlDmd9b2VcQ=',
                  ['*.kt', '*.kts']),
                 ('@github:ktorio/ktor/3.6.1/sha256-iG988YK3dYlAKRJU0QIwvaW4obCkCySWldCIq3BrVGo=', ['*.kt', '*.kts']),
                 ('@github:square/okhttp/parent-5.5.1/sha256-e7sBjP7ita2j/TyR+ZdMvzxgUjqsmnPF85KFgtv3P9w=',
                  ['*.kt', '*.kts']),
                 ('ktfmt.src', ['*.kt', '*.kts'])],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-kotlin.src', 'src'),
        oracle=[str(REPO / 'build' / 'grammar-diff' / 'kotlin' / 'kotlin-check'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '"', '"""', "'", '${', '$', '/*', '*/', '//', ';', '\n', '::', '?.',
                '?:', '!!', '->', '..', '..<', '@', 'fun ', 'val ', 'var ', 'class ', 'object ', 'when ', 'if (',
                'else ', 'is ', 'as ', 'in ', '<T>', '<', '>', '0x', '1e', '`'],
    ),
    # D: LDC 1.41's source tree -- Phobos, druntime, the dmd front end
    # itself and dmd's test suite (tests/dmd; fail_compilation is
    # deliberately invalid) -- then dub, D-Scanner and serve-d from nixpkgs,
    # and DCD, libdparse, dfmt and vibe.d from GitHub. The old grammar is
    # gdamore/tree-sitter-d from flake.lock's nixpkgs. The oracle is the
    # compiler's own parser (d_check.sh: `ldc2 -o- -v`, a syntax error is
    # one reported before importall; `just d-check` links the ldc).
    'd': dict(
        globs=['*.d', '*.di'],
        sources=[('@pinned:ldc.src', ['runtime/*.d', 'runtime/*.di', 'dmd/*.d', 'tests/*.d', 'tests/*.di'])] + [
            (f'{p}.src', ['*.d', '*.di']) for p in ['dub', 'dscanner', 'serve-d']] + [
            ('@github:dlang-community/DCD/4946d49abdc35810254151923bab30fb3cc2c004/'
             'sha256-dJ4Ql3P9kPQhQ3ZrNcTAEB5JHSslYn2BN8uqq6vGetY=', ['*.d']),
            ('@github:dlang-community/libdparse/f8a6c28589aae180532fb460a1b22e92a0978292/'
             'sha256-D+fFf/JjL3H39qsbfiS6A6zvz5272m/HAXVYmFYVSPo=', ['*.d']),
            ('@github:dlang-community/dfmt/d8e43e23eca0aa32f064fe7efe8e74a9efa8018e/'
             'sha256-QjmYPIQFs+91jB1sdaFoenfWt5TLXyEJauSSHP2fd+M=', ['*.d']),
            ('@github:vibe-d/vibe.d/faf66b99de605b8a4eb8af37bc8b49f0f0e1c922/'
             'sha256-/V1Ez1W7fSquVWZG4RfhZxOOxO+i8R5u9+343VAzE1E=', ['*.d'])],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-d.src', 'src'),
        oracle=[str(Path(__file__).resolve().parent / 'd_check.sh'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '"', "'", '`', '/*', '*/', '/+', '+/', '//', ';', '\n', 'q{', 'q"(',
                'r"', 'x"', 'i"', '$(', '!', '!(', '=>', '..', '...', '@', 'static if (', 'version (', 'mixin(',
                'template ', 'class ', 'struct ', 'enum ', 'alias ', 'auto ', 'scope(exit) ', 'in {', 'out(r) {',
                '__traits(', 'asm {', '0x', '1e', '__EOF__'],
    ),
    # Zig: the Zig 0.16 tree's own lib/ (std, compiler_rt, the C shims),
    # src/ (the compiler) and test/ (some deliberately invalid), then zls,
    # ghostty, river, waylock, ncdu and linuxwave from nixpkgs, and
    # tigerbeetle's and bun's sources from GitHub.
    # The old grammar is tree-sitter-zig (tree-sitter-grammars) from
    # flake.lock's nixpkgs. The oracle is Zig's own parser, std.zig.Ast, as
    # `zig fmt --stdin` runs it (zig_check.sh): parse only, unlike `zig
    # ast-check`, whose AstGen errors (unused locals ...) are no syntax error.
    'zig': dict(
        globs=['*.zig'],
        sources=[('@pinned:zig.src', ['lib/*.zig', 'src/*.zig', 'test/*.zig'])] + [
            (f'{p}.src', ['*.zig']) for p in ['zls', 'ghostty', 'river', 'waylock', 'ncdu', 'linuxwave']] + [
            # nixpkgs packages these two as release binaries: their sources.
            ('@github:tigerbeetle/tigerbeetle/0.17.9/sha256-LgiVqyQCIIrvpm9Jg3g8DjmX9eEYye36SARHZTTwJvc=',
             ['*.zig']),
            ('@github:oven-sh/bun/bun-v1.3.13/sha256-sN3HnhI2kNc+P1E5MnE6A0VyMAk2dwPp+uITZiPdfKM=', ['*.zig'])],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-zig.src', 'src'),
        oracle=[str(Path(__file__).resolve().parent / 'zig_check.sh'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '"', "'", '\\\\', '//', ';', '\n', '.{', '.*', '.?', '..', '...',
                '=>', '|', 'fn ', 'const ', 'var ', 'pub ', 'struct {', 'enum {', 'union(enum) {', 'switch (', 'try ',
                'catch ', 'orelse ', 'comptime ', '@import(', 'error{', '!', '?', '0x', '1e'],
    ),
    # Nim: the Nim 2.2.10 tree's own lib/ (the standard library), compiler/,
    # nimsuggest/, nimpretty/, tools/, testament/ and tests/ (some
    # deliberately invalid), then Nim programs nixpkgs packages (nimble,
    # nimlangserver, nimlsp, nph, nitter, c2nim, nimmm) and karax and jester
    # from GitHub. The old grammar is alaviss/tree-sitter-nim (MPL-2.0) from
    # flake.lock's nixpkgs. The oracle is Nim's own parser,
    # compiler/parser.nim, with nothing else run (nim_check.nim; `just
    # nim-check`).
    'nim': dict(
        globs=['*.nim', '*.nims', '*.nimble'],
        sources=[('@pinned:nim-unwrapped.src', [f'nim-*/{d}/*.{e}' for d in
                                                ['lib', 'compiler', 'nimsuggest', 'nimpretty', 'tools', 'testament',
                                                 'tests'] for e in ['nim', 'nims', 'nimble']])] + [
            (f'{p}.src', ['*.nim', '*.nims', '*.nimble'])
            for p in ['nimble', 'nimlangserver', 'nimlsp', 'nph', 'nitter', 'c2nim', 'nimmm']] + [
            ('@github:karaxnim/karax/1.5.0/sha256-U82TGbXwsCFFL1idRvRjGsI9ysuqF4bchaiP9ANmw/c=',
             ['*.nim', '*.nims', '*.nimble']),
            ('@github:dom96/jester/v0.6.0/sha256-F/zWWGipJ4lBE3njceXn5HBFTYEXB4l2rk6+FfqqZTQ=',
             ['*.nim', '*.nims', '*.nimble'])],
        filter_globs=True,
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-nim.src', 'src'),
        oracle=[str(REPO / 'build' / 'grammar-diff' / 'nim' / 'nim-check'), '{}'],
        tokens=['(', ')', '[', ']', '{', '}', '{.', '.}', '"', '"""', "'", '`', '#', '#[', ']#', '##', '\n', '  ',
                ':', '=', ',', ';', '.', '..', '*', 'proc ', 'type ', 'var ', 'let ', 'const ', 'if ', 'elif ',
                'else:', 'case ', 'of ', 'object', 'ref ', 'tuple[', 'do:', 'when ', 'template ', 'macro ', 'r"',
                '0x', '1e', "'u8"],
    ),
    # Rust: rustc 1.97's own tree -- the standard library (core, alloc, std
    # and the crates vendored under library/), the compiler, rustfmt, clippy's
    # lints, rust-analyzer's crates and cargo -- plus widely used crates from
    # its vendor/ directory (one version each) and Rust programs nixpkgs
    # packages. The old grammar is tree-sitter-rust 0.24.2 from flake.lock's
    # nixpkgs. The oracle is rustc's own parser (`-Zparse-crate-root-only`,
    # rust_check.py), at the edition the file's crate declares.
    'rust': dict(
        globs=['*.rs'],
        sources=[('@pinned:rustc.unwrapped.src',
                  ['rustc-*-src/library/*.rs', 'rustc-*-src/compiler/*.rs', 'rustc-*-src/src/tools/rustfmt/src/*.rs',
                   'rustc-*-src/src/tools/clippy/clippy_lints/*.rs', 'rustc-*-src/src/tools/rust-analyzer/crates/*.rs',
                   'rustc-*-src/src/tools/cargo/src/*.rs', 'rustc-*-src/*/Cargo.toml', 'rustc-*-src/Cargo.toml']
                  + [f'rustc-*-src/vendor/{c}/*.rs' for c in VENDORED_RUST_CRATES]
                  + [f'rustc-*-src/vendor/{c}/Cargo.toml' for c in VENDORED_RUST_CRATES])] + [
            (f'{p}.src', ['*.rs', '*Cargo.toml']) for p in ['ripgrep', 'fd', 'bat', 'helix']],
        # Cargo.toml files are unpacked beside the sources (rust_check.py reads
        # each crate's edition from them) but are not corpus files.
        filter_globs=True,
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-rust.src', 'src'),
        oracle=['python3', '-I', str(Path(__file__).resolve().parent / 'rust_check.py'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '<', '>', '"', "'", 'r#"', '"#', 'b"', '/*', '*/', '//', '///', ';', '\n',
                '::', '->', '=>', '#[', '#![', '!', '?', '&', "'a", '|', '..', 'fn ', 'impl ', 'let ', 'match ',
                'macro_rules! ', 'where ', 'async ', 'unsafe ', '0x', '1e', '1u8'],
    ),
    'tcl': dict(
        globs=['*.tcl', '*.test', '*.tm'],
        sources=[('tcl.src', ['*/library/*.tcl', '*/library/*.tm', '*/tests/*.test', '*/tests/*.tcl',
                              '*/tools/*.tcl']),
                 ('tk.src', ['*/library/*.tcl', '*/tests/*.test', '*/tests/*.tcl']),
                 ('tclPackages.tcllib.src', ['*.tcl', '*.test', '*.tm'])],
        dedupe=True,
        old=('tree-sitter-grammars.tree-sitter-tcl.src', 'src'),
        oracle=['tclsh', str(Path(__file__).resolve().parent / 'tcl_check.tcl'), '{}'],
        tokens=['{', '}', '[', ']', '"', '$', '${', '\\', '\\\n', ';', '#', '{*}', '(', ')', '\n', 'proc ', 'if ',
                'expr {', ' else '],
    ),
    # Octave/MATLAB (.m): Octave's own scripts, tests and examples, then
    # Octave Forge packages (some written as plain MATLAB). The old grammar
    # is nixpkgs' tree-sitter-matlab (acristoffers, MATLAB only). The oracle
    # is Octave's own parser: octave_check.sh runs __parse_file__, which
    # parses a function file, script or classdef and runs nothing.
    'matlab': dict(
        globs=['*.m'],
        sources=[('octave.src', ['*/scripts/*.m', '*/test/*.m', '*/examples/*.m'])] + [
            (f'octavePackages.{p}.src', ['*.m'])
            for p in ['statistics', 'signal', 'control', 'image', 'io', 'optim', 'symbolic', 'geometry',
                      'matgeom', 'nurbs', 'interval', 'communications', 'financial', 'mapping', 'ltfat',
                      'general', 'miscellaneous', 'queueing', 'stk', 'struct', 'strings', 'tsa', 'nan',
                      'splines', 'fuzzy-logic-toolkit', 'bim', 'msh', 'fpl', 'datatypes', 'dataframe']],
        dedupe=True,
        old=('tree-sitter-grammars.tree-sitter-matlab.src', 'src'),
        oracle=[str(Path(__file__).resolve().parent / 'octave_check.sh'), '{}'],
        tokens=['%', '#', '%{\n', '%}\n', '#{\n', '#}\n', "'", '"', '...', '...\n', '[', ']', '{', '}', '(', ')',
                ';', ',', '\n', 'end', 'endfunction', 'endif', 'function ', 'if ', 'classdef ', "x'", '.^', '!=',
                '~', '@(', ':', '\\\n'],
    ),
    # Fortran, free form (.f90 ...) and fixed form (.f/.for): reference
    # LAPACK/BLAS, scipy's and ARPACK's Fortran, Quantum ESPRESSO, SIESTA,
    # Octopus, Wannier90, netCDF-Fortran and HDF5's Fortran API, plus
    # gfortran's own test suite (gcc 14), which is full of edge cases and
    # deliberately invalid files. The old grammar is nixpkgs'
    # tree-sitter-fortran (stadelmanma). The oracle is gfortran
    # -fsyntax-only behind fortran_check.py, which keeps only syntax-class
    # errors (modules a file USEs are built from the corpus, or stubbed).
    'fortran': dict(
        globs=['*.f90', '*.F90', '*.f95', '*.F95', '*.f03', '*.F03', '*.f08', '*.F08', '*.f', '*.F', '*.for',
               '*.FOR'],
        sources=[(a, ['*.f90', '*.F90', '*.f95', '*.F95', '*.f03', '*.F03', '*.f08', '*.F08', '*.f', '*.F',
                      '*.for', '*.FOR'])
                 for a in ['lapack-reference.src', 'python3Packages.scipy.src', 'arpack.src', 'wannier90.src',
                           'quantum-espresso.src', 'netcdffortran.src', 'hdf5-fortran.src', 'siesta.src',
                           'octopus.src']] + [
            ('gfortran.cc.src', ['*/gcc/testsuite/gfortran.dg/*.f90', '*/gcc/testsuite/gfortran.dg/*.F90',
                                 '*/gcc/testsuite/gfortran.dg/*.f', '*/gcc/testsuite/gfortran.dg/*.F',
                                 '*/gcc/testsuite/gfortran.dg/*.f03', '*/gcc/testsuite/gfortran.dg/*.f08'])],
        dedupe=True,
        old=('tree-sitter-grammars.tree-sitter-fortran.src', 'src'),
        oracle=['python3', '-I', str(Path(__file__).resolve().parent / 'fortran_check.py'), '{}'],
        tokens=['&', '&\n', '\n', '!', "'", '"', '(', ')', '(/', '/)', '[', ']', '::', ';', '%', '=>', '.and.',
                'end', 'end do', 'end if', 'subroutine ', 'function ', 'module ', 'type ', 'if (', ') then',
                'contains\n', '#ifdef X\n', '#endif\n', '\n     $', '\nC ', '\n*'],
    ),
    # GAP: its library (lib/, grp/, hpcgap/) and the ~160 packages bundled
    # with it, plus the input lines of every .tst file (test transcripts:
    # `gap> ` / `> ` lines, extracted per file by gap_test_inputs). The old
    # grammar is gap-system/tree-sitter-gap, which mep built from GitHub in
    # flake.nix (no nixpkgs attribute). The oracle is GAP's own reader
    # (gap_check.py): ReadAsFunction, which parses without running, with the
    # file's top-level expression statements and quit/? lines located by
    # mep-gap-lsp (--toplevel; $MEP_GAP_LSP overrides build/native's).
    'gap': dict(
        globs=['*.g', '*.gi', '*.gd'],
        sources=[('gap.src', ['*.g', '*.gi', '*.gd']), ('@gap-tests', [])],
        dedupe=True,
        old=('@github:gap-system/tree-sitter-gap/96fe2e49745ecd62b80cd19dca01fb52b83f93a1/'
             'sha256-bHbWHfwzQVnbqNZ65Ls2h3hsnIpQpylzy2Sviw8aTcs=', 'src'),
        oracle=['python3', '-I', str(Path(__file__).resolve().parent / 'gap_check.py'), '--toplevel',
                os.environ.get('MEP_GAP_LSP', str(REPO / 'build' / 'native' / 'mep-gap-lsp')), '{}'],
        tokens=['"', '"""', "'", '\\', '\\\n', '#', ';', ':=', '->', 'function(', 'end', 'fi', 'od', 'do', 'then',
                'local ', 'rec(', '[', ']', '(', ')', '{', '}', '..', '.', '!.', '![', '~', 'atomic ', '\n'],
    ),
    'lua': dict(
        globs=['*.lua'],
        sources=[(a, ['*.lua']) for a in ['luarocks.src', 'awesome.src', 'lua-language-server.src',
                                          'vimPlugins.telescope-nvim.src', 'vimPlugins.nvim-treesitter.src',
                                          'vimPlugins.lazy-nvim.src']],
        oracle=['luac', '-p', '{}'],
        tokens=['[[', ']]', '[==[', ']==]', '--[[', 'end', 'function', '::', '...'],
    ),
    'c': dict(
        # mep's own headers (src/*.h) are C++; only third_party's are C.
        globs=['*.c', 'third_party/*.h'],
        sources=[('sqlite.src', ['*.c', '*.h']), ('lua5_4.src', ['*.c', '*.h']),
                 ('python3.src', ['*/Objects/*.c', '*/Python/*.c', '*/Include/*.h', '*/Modules/*.c'])],
        # A C file's validity depends on its include path and macros, so a
        # compiler run on it alone says little: no oracle.
        oracle=None,
        tokens=['#if', '#endif', '#define', '\\\n', ';', '*/', 'struct ', '->'],
    ),
    'cpp': dict(
        globs=['*.cpp', '*.cc', '*.cxx', '*.hpp', '*.hh', '*.hxx', '*.h'],
        sources=[('fmt.src', ['*.cc', '*.h']), ('abseil-cpp.src', ['*.cc', '*.h']),
                 ('nlohmann_json.src', ['*/include/*.hpp'])],
        oracle=None,
        tokens=['R"x(', ')x"', '<', '>', '>>', '::', 'template<', '#if', '#endif', ';'],
    ),
    'python': dict(
        globs=['*.py', '*.pyi'],
        sources=[('python3.src', ['*/Lib/*.py'])] + [
            (f'python3Packages.{p}.src', ['*.py', '*.pyi']) for p in ('numpy', 'django', 'requests', 'sympy')],
        oracle=['python3', '-c', 'import ast,sys; ast.parse(open(sys.argv[1], "rb").read())', '{}'],
        tokens=['f"', 'f"{', '"""', ':\n', '\n    ', '\n\t', 'lambda', 'match ', '\\\n'],
    ),
    'javascript': dict(
        globs=['*.js', '*.mjs', '*.cjs', '*.jsx'],
        # Three React front ends for JSX (Node's lib/ has none).
        sources=[('nodejs.src', ['*/lib/*.js', '*/lib/*.mjs']),
                 ('mastodon.src', ['app/javascript/*.js', 'app/javascript/*.jsx']),
                 ('homepage-dashboard.src', ['src/*.js', 'src/*.jsx']),
                 ('navidrome.src', ['ui/src/*.js', 'ui/src/*.jsx'])],
        # node rejects JSX: esbuild (strict, and parses JSX) checks .jsx.
        oracle=['sh', '-c', 'case "$1" in *.jsx) exec nix shell nixpkgs#esbuild --command '
                'esbuild --loader=jsx --log-level=error "$1" >/dev/null;; '
                '*) exec node --check "$1";; esac', 'sh', '{}'],
        tokens=['`', '${', '/', '/g', '=>', '<div>', '</div>', ';', '\n'],
    ),
    # TypeScript's two dialects. The old grammars were never vendored: mep
    # loaded nixpkgs' tree-sitter-typescript/-tsx, whose source ('old') is
    # the reference. TypeScript's own parser is the oracle (ts_check.js).
    'typescript': dict(
        globs=['*.ts', '*.mts', '*.cts'],
        # TypeScript's compiler, services and lib .d.ts files, and two
        # TypeScript front ends' non-JSX code (Outline, Mastodon).
        sources=[('typescript.src', ['src/*.ts']),
                 ('outline.src', ['app/*.ts', 'server/*.ts', 'shared/*.ts', 'plugins/*.ts']),
                 ('mastodon.src', ['app/javascript/*.ts'])],
        old=('tree-sitter-grammars.tree-sitter-typescript.src', 'typescript/src'),
        oracle=['nix', 'shell', 'nixpkgs#nodejs', 'nixpkgs#typescript', '--command', 'node',
                str(Path(__file__).resolve().parent / 'ts_check.js'), '{}'],
        tokens=['`', '${', '<T>', '<', '>', ': ', '?', '!', '=>', 'as ', 'type ', 'interface ', 'declare ', ';',
                '\n'],
    ),
    'tsx': dict(
        globs=['*.tsx'],
        sources=[('outline.src', ['app/*.tsx', 'server/*.tsx', 'shared/*.tsx', 'plugins/*.tsx']),
                 ('mastodon.src', ['app/javascript/*.tsx'])],
        old=('tree-sitter-grammars.tree-sitter-tsx.src', 'tsx/src'),
        oracle=['nix', 'shell', 'nixpkgs#nodejs', 'nixpkgs#typescript', '--command', 'node',
                str(Path(__file__).resolve().parent / 'ts_check.js'), '{}'],
        tokens=['`', '${', '<div>', '</div>', '<T,>', '<', '>', ': ', '?', '!', '=>', 'as ', '{', '}', ';', '\n'],
    ),
    # fish: its own functions, completions and test scripts, then the fish
    # plugins nixpkgs packages. mep loaded nixpkgs' tree-sitter-fish
    # (ram02z) before; its source ('old') is the reference. fish's own
    # parser is the oracle (`--no-execute` parses, reports, runs nothing).
    # Julia: Base, the standard library, Julia's own tests and the compiler
    # (julia-bin ships their sources), plus registry packages as nixpkgs'
    # julia.withPackages fetches them. The old grammar is tree-sitter-julia
    # 0.25.0 from flake.lock's nixpkgs ('@pinned:'). The oracle is
    # JuliaSyntax, the parser behind Meta.parseall (julia_check.jl).
    'julia': dict(
        globs=['*.jl'],
        sources=[('julia-bin', ['share/julia/base/*.jl', 'share/julia/stdlib/*.jl', 'share/julia/test/*.jl',
                                'share/julia/Compiler/*.jl']),
                 ('@julia-packages', ['*.jl'])],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-julia.src', 'src'),
        oracle=['julia', '--startup-file=no', '--history-file=no',
                str(Path(__file__).resolve().parent / 'julia_check.jl'), '{}'],
        tokens=['end', 'begin', 'function ', '"""', '"', "'", '`', '```', '$(', '#=', '=#', '(', ')', '[', ']', '{',
                '}', ';', ',', '\n', '::', '...', "'", '.', '@', ':', 'do ', '->', 'where ', '\\'],
    ),
    # Java: JDK 25's own module sources (src.zip of flake.lock's jdk25:
    # java.base, the desktop, XML, compiler, JFR, HTTP, SQL, jshell and
    # javadoc modules), then guava, spring-framework, commons-lang, JUnit 5,
    # Kafka and Elasticsearch's server/ and libs/, cloned at pinned commits
    # ('@git:'). The old grammar is tree-sitter-java 0.23.5 from flake.lock's
    # nixpkgs. The oracle is javac's own parser (JavaCheck.java, JDK 25's
    # JavacTask.parse(); `just java-check`).
    'java': dict(
        globs=['*.java'],
        sources=[('@zip:jdk25:lib/openjdk/lib/src.zip', [
            f'{m}/*.java' for m in ['java.base', 'java.desktop', 'java.xml', 'jdk.compiler', 'java.management',
                                    'jdk.jfr', 'java.net.http', 'java.sql', 'jdk.jshell', 'jdk.javadoc',
                                    'java.compiler', 'jdk.httpserver', 'java.logging', 'java.naming']])] + [
            (f'@git:https://github.com/{repo}@{rev}', pats) for repo, rev, pats in [
                ('google/guava', 'ed0921b633d73d5a1d32c8c147784ca7add48757', ['*.java']),
                ('spring-projects/spring-framework', 'f95aca035c97d3a80bb4679877784b728d037ca4', ['*.java']),
                ('apache/commons-lang', '42bd1f218caf86df139b08d010334e97392af80d', ['*.java']),
                ('junit-team/junit5', '11fbe0ec4825b50890fefd441e0d5fd8738d477c', ['*.java']),
                ('apache/kafka', '38faa94fe5b68fe8390c6968e872318b2b6cadfa', ['*.java']),
                ('elastic/elasticsearch', '87860ac45d16e216fe893a558684f478736cbabe',
                 ['server/*.java', 'libs/*.java'])]],
        dedupe=True,
        old=('@pinned:tree-sitter-grammars.tree-sitter-java.src', 'src'),
        oracle=[str(REPO / 'build' / 'grammar-diff' / 'java' / 'java-check'), '{}'],
        tokens=['{', '}', '(', ')', '[', ']', '<', '>', '"', "'", '"""', '/*', '*/', '/**', '//', ';', '\n', '->',
                '::', '...', '.', '@', 'class ', 'record ', 'enum ', 'interface ', 'case ', 'default:', 'new ',
                'yield ', 'sealed ', 'permits ', 'when ', '0x', '1e', '\\u0022'],
    ),
    'fish': dict(
        globs=['*.fish'],
        sources=[('fish.src', ['*.fish'])] + [
            (f'fishPlugins.{p}.src', ['*.fish'])
            for p in ['tide', 'pure', 'hydro', 'bobthefish', 'fzf-fish', 'forgit', 'done', 'z', 'bass',
                      'autopair', 'plugin-git', 'puffer', 'sponge', 'colored-man-pages', 'foreign-env', 'fifc',
                      'async-prompt', 'fishtape_3', 'transient-fish', 'git-abbr', 'sdkman-for-fish', 'nvm',
                      'humantime-fish', 'fish-you-should-use', 'spark', 'grc', 'gruvbox', 'clownfish']],
        dedupe=True,
        old=('tree-sitter-grammars.tree-sitter-fish.src', 'src'),
        oracle=['fish', '--no-execute', '{}'],
        tokens=['end', 'begin', '; ', '\n', '(', ')', '$(', '"', "'", '{', '}', ',', '$', '[', ']', '..', '|',
                '&&', '>', '2>&1', '\\\n', '#'],
    ),
    'commonlisp': dict(
        globs=['*.lisp', '*.lsp', '*.cl', '*.asd'],
        # SBCL's own sources (compiler, runtime library, contribs) and a few
        # widely used libraries with heavy reader-macro and backquote use.
        sources=[('sbcl.src', ['*.lisp', '*.asd'])] + [
            (f'sbclPackages.{p}.src', ['*.lisp', '*.asd'])
            for p in ('alexandria', 'cl-ppcre', 'iterate', 'trivia', 'cffi', 'fiveam')],
        # Reading a file needs its packages to exist (`pkg:sym`) and its
        # reader macros installed, so the reader alone says little: no oracle.
        oracle=None,
        tokens=['#|', '|#', '#\\', '#+', '#-', ',@', '`', '|', '\\', '#(', "#'", ';', ':'],
    ),
    'bash': dict(
        globs=['*.sh', '*.bash'],
        # bash-completion (large, dense, every expansion form), git's test
        # suite (heredocs everywhere), bash's own examples and tests,
        # nixpkgs' setup hooks and builders, bats, asdf and dehydrated.
        sources=[('bash-completion.src', ['*/completions/[!M]*', '*/bash_completion', '*.sh', '*.bash']),
                 ('git.src', ['*/t/*.sh', '*.sh']),
                 ('bash.src', ['*/examples/*', '*/tests/*.sub', '*/tests/*.tests', '*.sh']),
                 ('@nixpkgs', ['*.sh']),
                 ('bats.src', ['*.bash', '*.sh', '*/libexec/*', '*/bin/*']),
                 ('asdf-vm.src', ['*.bash', '*.sh']),
                 ('dehydrated.src', ['*/dehydrated', '*.sh'])],
        # extglob on: `@(a|b)` is a parse error without it, and scripts
        # that use it turn it on at the top.
        oracle=['bash', '-O', 'extglob', '-n', '{}'],
        tokens=['<<EOF\n', '\nEOF\n', '<<-', '$(', '$((', '))', '${', '}', '[[', ']]', '`', 'esac', ';;', 'fi',
                'done', '=~', '@(', "$'", '\\\n'],
    ),
    'clojure': dict(
        globs=['*.clj', '*.cljs', '*.cljc', '*.edn', '*.bb'],
        # Clojure libraries ship as source in their jars, so the tools'
        # uberjars are a corpus: Clojure's own core, ClojureScript bits,
        # clj-kondo, rewrite-clj, babashka's libraries, polylith, jet, cljfmt.
        sources=[(f'{p}.src', ['*.clj', '*.cljs', '*.cljc', '*.edn', '*.bb'])
                 for p in ['clojure-lsp', 'babashka-unwrapped', 'clj-kondo', 'polylith', 'jet', 'cljfmt', 'neil']],
        dedupe=True,
        # babashka's edamame: a reader that reads without evaluating, with
        # every reader feature on (reader conditionals, `#=`, any tag).
        oracle=['bb', str(Path(__file__).resolve().parent / 'clojure_read.clj'), '{}'],
        tokens=['(', ')', '[', ']', '{', '}', '#{', '#(', '#?(', '#_', '^', '"', '\\', ':', '::', '@', '~@', '`', ';'],
    ),
    'crystal': dict(
        globs=['*.cr'],
        # The compiler and standard library (and their specs: every corner
        # of the syntax), then the Crystal programs packaged in nixpkgs (rtfm
        # vendors its shards, generated GTK bindings among them).
        sources=[('crystal.src', ['*.cr'])] + [
            (f'{p}.src', ['*.cr'])
            for p in ['crystalline', 'ameba', 'ameba-ls', 'mint', 'invidious', 'lucky-cli', 'shards', 'oq',
                      'crystal2nix', 'icr', 'thicket', 'kakoune-cr', 'gi-crystal', 'collision', 'blahaj', 'rtfm',
                      'tmuxPlugins.fingers']],
        dedupe=True,
        # The formatter parses without type-checking: a syntax error is the
        # parser's, and nothing else fails before formatting (crystal_check.sh).
        oracle=['sh', str(Path(__file__).resolve().parent / 'crystal_check.sh'), '{}'],
        tokens=['<<-EOS\n', '\nEOS\n', '%w(', '%(', '%q[', '%r{', '#{', '{{', '}}', '{%', '%}', ' end', 'do |x|',
                ' ? ', ' : ', '/', '@[', '->', '::', "'"],
    ),
    'c_sharp': dict(
        globs=['*.cs'],
        # Roslyn's own compiler, workspaces and tests (every corner of the
        # syntax, C# 13), then C# applications nixpkgs packages: a media
        # server, games, a web app, a decompiler, a wallet, the OmniSharp
        # language server.
        sources=[('roslyn.src', ['src/Compilers/*.cs', 'src/Analyzers/*.cs'])] + [
            (f'{p}.src', ['*.cs'])
            for p in ['jellyfin', 'ArchiSteamFarm', 'osu-lazer', 'openra', 'pinta', 'omnisharp-roslyn', 'ilspycmd',
                      'btcpayserver', 'depotdownloader', 'wasabibackend', 'inklecate', 'scarab']],
        dedupe=True,
        # Roslyn's parser, the copy the .NET SDK ships (tools/csharp_oracle;
        # build it with `just csharp-oracle`): exit 1 on a syntax error.
        oracle=['dotnet', str(REPO / 'build' / 'csharp-oracle' / 'csharp-oracle.dll'), 'check', '{}'],
        tokens=['{', '}', '(', ')', ';', '<', '>', '=>', '$"', '@"', '"""', '#if', '#endif', '?', '?.', ':',
                'where', 'async ', 'await ', '[', ']', 'new ', 'switch'],
    ),
    'sql': dict(
        globs=['*.sql'],
        # sqlfluff's dialect fixtures (each a statement set its own parser
        # reads: ANSI, PostgreSQL, MySQL/MariaDB, SQLite, T-SQL, BigQuery,
        # Snowflake, Spark, Oracle... 30 dialects), then PostgreSQL's
        # regression and contrib tests (psql scripts: `\set`, `\d` and
        # COPY data lines included, as people's .sql files have them).
        sources=[('sqlfluff.src', ['test/fixtures/dialects/*.sql']),
                 ('postgresql.src', ['*.sql'])],
        dedupe=True,
        # SQL has no one reference parser. sql_check.py accepts a file
        # SQLite parses (executed statement by statement into :memory:,
        # only syntax errors count) or PostgreSQL's parser does (pglast,
        # when importable); a file only another dialect accepts is
        # "rejected", so the gate is --old-clean.
        oracle=['python3', '-I', str(Path(__file__).resolve().parent / 'sql_check.py'), '{}'],
        tokens=['$$', '$tag$', "'", '"', '`', '[', ']', '(', ')', ';', '--', '/*', '*/', 'BEGIN', 'END', 'CASE ',
                'SELECT ', 'FROM ', 'WHERE ', ',', '::', 'E\''],
    ),
    'org': dict(
        globs=['*.org'],
        # Emacs's Org manual and friends, and four Org packages' docs and
        # test files (ox-hugo's test site is 55 files of every feature).
        sources=[('emacs.src', ['*.org']), ('emacsPackages.ox-hugo.src', ['*.org']),
                 ('emacsPackages.org-roam.src', ['*.org']), ('emacsPackages.org-ql.src', ['*.org']),
                 ('emacsPackages.org-super-agenda.src', ['*.org'])],
        oracle=None,
        tokens=['* ', '** ', '#+begin_src', '#+end_src', ':PROPERTIES:', ':END:', '[[', ']]', '|', '- [ ] '],
    ),
    'markdown': dict(
        globs=['*.md', '*.markdown'],
        # Plus every example of the CommonMark and GFM specs (and both
        # suites' regression and extension tests), one file each: the edge
        # cases the specs were written to pin down.
        sources=[('cmark.src', ['*.md']), ('nodejs.src', ['*/doc/*.md']), ('@spec-examples', [])],
        # Markdown accepts any text, so its oracle compares structure: the
        # blocks cmark-gfm (the reference implementation, with GFM's tables
        # and task lists) reads, against the tree's (markdown_skeleton).
        oracle=None,
        structure_oracle='markdown',
        tokens=['```', '~~~', '# ', '> ', '- ', '1. ', '*', '_', '[', '](', '<', '|', '\n\n'],
    ),
}
LANGUAGES['markdown_inline'] = LANGUAGES['markdown']


def die(message):
    print(f'grammar_diff: {message}', file=sys.stderr)
    sys.exit(2)


def run(argv, **kw):
    return subprocess.run(argv, check=True, **kw)


def language(name):
    if name not in LANGUAGES:
        die(f'unknown grammar {name!r}; one of: {", ".join(sorted(LANGUAGES))}')
    return LANGUAGES[name]


# ---- corpus -----------------------------------------------------------------

def nix_out_path(attr):
    if attr.startswith('@maven:'):
        # A Maven Central artifact (a sources jar): fetchurl, fixed-output,
        # rooted like the GitHub ones. group/artifact/version/classifier/hash.
        group, artifact, version, classifier, sha = attr[len('@maven:'):].split('/', 4)
        url = (f'https://repo1.maven.org/maven2/{group.replace(".", "/")}/{artifact}/{version}/'
               f'{artifact}-{version}-{classifier}.jar')
        expr = (f'(builtins.getFlake "nixpkgs").legacyPackages.${{builtins.currentSystem}}.fetchurl '
                f'{{ url = "{url}"; hash = "{sha}"; }}')
        root = CORPUS / '.gcroots' / f'maven-{artifact}-{version}-{classifier}'
        root.parent.mkdir(parents=True, exist_ok=True)
        return subprocess.run(['nix', 'build', '--impure', '--out-link', str(root), '--print-out-paths', '--expr', expr],
                              check=True, capture_output=True, text=True).stdout.strip()
    if attr.startswith('@github:'):
        # A grammar built straight from GitHub (flake.nix's fetchFromGitHub):
        # the same fixed-output path, so it is found in the store offline.
        owner, repo, rev, sha = attr[len('@github:'):].split('/', 3)
        expr = (f'(builtins.getFlake "nixpkgs").legacyPackages.${{builtins.currentSystem}}.fetchFromGitHub '
                f'{{ owner = "{owner}"; repo = "{repo}"; rev = "{rev}"; hash = "{sha}"; }}')
        # Rooted, so a garbage collection doesn't empty a corpus built from it.
        root = CORPUS / '.gcroots' / f'github-{owner}-{repo}-{rev}'
        root.parent.mkdir(parents=True, exist_ok=True)
        return subprocess.run(['nix', 'build', '--impure', '--out-link', str(root), '--print-out-paths', '--expr', expr],
                              check=True, capture_output=True, text=True).stdout.strip()
    if attr == '@nixpkgs':
        return subprocess.run(['nix', 'eval', '--raw', '--impure', '--expr', '(builtins.getFlake "nixpkgs").outPath'],
                              check=True, capture_output=True, text=True).stdout.strip()
    if attr.startswith('@pinned:'):
        # flake.lock's nixpkgs, not the registry's: the grammar mep's flake
        # actually loaded (the registry's tree-sitter-julia is a different,
        # older revision with another node API).
        attr = attr[len('@pinned:'):]
        root = CORPUS / '.gcroots' / f'pinned-{attr}'
        root.parent.mkdir(parents=True, exist_ok=True)
        return subprocess.run(['nix', 'build', '--out-link', str(root), '--print-out-paths', f'nixpkgs#{attr}',
                               '--inputs-from', str(REPO)],
                              check=True, capture_output=True, text=True).stdout.strip().splitlines()[0]
    if attr == '@julia-packages':
        # Julia packages from the General registry, as nixpkgs'
        # julia.withPackages fetches them (fixed-output, no precompiling):
        # the depot's packages/ directory.
        names = ' '.join(f'"{p}"' for p in JULIA_PACKAGES)
        expr = ('let p = (builtins.getFlake "nixpkgs").legacyPackages.${builtins.currentSystem}; in '
                f'(p.julia-bin.withPackages.override {{ precompile = false; }}) [ {names} ]')
        root = CORPUS / '.gcroots' / 'julia-packages'
        root.parent.mkdir(parents=True, exist_ok=True)
        env = subprocess.run(['nix', 'build', '--impure', '--out-link', str(root), '--print-out-paths', '--expr', expr],
                             check=True, capture_output=True, text=True).stdout.strip().splitlines()[0]
        wrapper = Path(env, 'bin', 'julia').read_text()
        depot = re.search(r'(/nix/store/[^/\'":]+-julia-depot)/depot', wrapper).group(1)
        return f'{depot}/depot/packages'
    # A GC root beside the corpus: a source directory is read in place, and
    # an unrooted store path can be garbage-collected between `corpus` and
    # `diff` (it was: half of Python's corpus, once).
    root = CORPUS / '.gcroots' / attr
    root.parent.mkdir(parents=True, exist_ok=True)
    argv = ['nix', 'build', '--out-link', str(root), '--print-out-paths', f'nixpkgs#{attr}']
    got = subprocess.run(argv, capture_output=True, text=True)
    if got.returncode != 0:
        # Gone from the registry's nixpkgs (tree-sitter-tcl was dropped
        # upstream): the one flake.lock pins still has it.
        got = subprocess.run(argv + ['--inputs-from', str(REPO)], check=True, capture_output=True, text=True)
    return got.stdout.strip().splitlines()[0]


SPEC_FILES = [('cmark.src', ['test/spec.txt', 'test/regression.txt']),
              ('cmark-gfm.src', ['test/spec.txt', 'test/regression.txt', 'test/extensions.txt'])]


def spec_examples(dest):
    """Each example of the CommonMark/GFM spec files as its own .md file
    (and its expected HTML beside it as .html), the spec's `→` back to the
    tab it stands for."""
    fence = '`' * 32
    got = []
    for attr, names in SPEC_FILES:
        root = Path(nix_out_path(attr))
        for name in names:
            text = (root / name).read_text(encoding='utf-8')
            stem = f'{attr.split(".")[0]}-{Path(name).stem}'
            n = 0
            for block in text.split(fence + ' example')[1:]:
                body = block.split('\n' + fence, 1)[0]
                body = body.split('\n', 1)[1] if '\n' in body else ''
                if '\n.\n' in '\n' + body:
                    md, html = ('\n' + body).split('\n.\n', 1)
                    md = md[1:] + '\n' if md.startswith('\n') else md
                else:
                    md, html = body, ''
                n += 1
                out = dest / f'{stem}-{n:04d}.md'
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_text(md.replace('\u2192', '\t'), encoding='utf-8')
                out.with_suffix('.html').write_text(html.replace('\u2192', '\t'), encoding='utf-8')
                got.append(str(out))
    return got


def matches(path, patterns):
    return any(fnmatch.fnmatch(path, p) or fnmatch.fnmatch(os.path.basename(path), p) for p in patterns)


def unpack(store_path, patterns, dest):
    """The source's files matching `patterns`: in place for a directory,
    extracted (data filter: no links, devices or absolute paths) for a
    tarball. Downloaded archives are untrusted, so nothing is run from them."""
    src = Path(store_path)
    if src.is_dir():
        return [str(p) for p in sorted(src.rglob('*')) if p.is_file() and matches(str(p.relative_to(src)), patterns)]
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    if zipfile.is_zipfile(src):
        # A jar: Clojure libraries ship as source inside them. Regular
        # files only, and no name that would land outside dest.
        with zipfile.ZipFile(src) as z:
            for info in z.infolist():
                name = info.filename
                if info.is_dir() or not matches(name, patterns):
                    continue
                parts = Path(name).parts
                if Path(name).is_absolute() or '..' in parts or (info.external_attr >> 16) & 0o170000 == 0o120000:
                    continue
                out = dest.joinpath(*parts)
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(z.read(info))
        return [str(p) for p in sorted(dest.rglob('*')) if p.is_file()]
    with tarfile.open(src) as tar:
        members = [m for m in tar.getmembers() if m.isfile() and matches(m.name, patterns)]
        tar.extractall(dest, members=members, filter='data')
    return [str(p) for p in sorted(dest.rglob('*')) if p.is_file()]


def just_test_justfiles(dest):
    """The justfiles embedded in just's own tests (tests/*.rs), un-indented
    the way its test harness does it."""
    src = Path(nix_out_path('just.src'))
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    out = []
    for rs in sorted(src.glob('tests/*.rs')):
        text = rs.read_text(errors='replace')
        for m in re.finditer(r'\.justfile\(\s*(?:indoc!\()?\s*(r#*)?"', text):
            start, raw = m.end(), m.group(1)
            if raw is not None:
                body = text[start:text.find('"' + raw[1:], start)]
            else:
                body, i = [], start
                escapes = {'n': '\n', 't': '\t', '\\': '\\', '"': '"'}
                while i < len(text) and text[i] != '"':
                    if text[i] == '\\' and i + 1 < len(text):
                        c = text[i + 1]
                        if c == '\n':
                            i += 2
                            while i < len(text) and text[i] in ' \t\n':
                                i += 1
                            continue
                        body.append(escapes.get(c, c))
                        i += 2
                    else:
                        body.append(text[i])
                        i += 1
                body = ''.join(body)
            if '{{' in body and 'format!' in text[max(0, m.start() - 200):m.start()]:
                continue  # a Rust format string, not a justfile
            path = dest / f'{len(out):04d}.just'
            path.write_text(textwrap.dedent(body).lstrip('\n'))
            out.append(str(path))
    return out


def gap_test_inputs(dest):
    """The GAP code in GAP's and its packages' test files (.tst): a test is
    a transcript, `gap> ` starting an input and `> ` continuing it, with
    the expected output between. Each .tst becomes one .g of its inputs, in
    order (line numbers are not kept)."""
    if dest.exists():
        shutil.rmtree(dest)
    raw = dest.parent / '_gap-tests-raw'
    tests = unpack(nix_out_path('gap.src'), ['*.tst'], raw)
    out = []
    for tst in tests:
        lines = []
        continuing = False
        for line in Path(tst).read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('gap>'):
                lines.append(line[5:] if line.startswith('gap> ') else line[4:])
                continuing = True
            elif continuing and line.startswith('>'):
                lines.append(line[2:] if line.startswith('> ') else line[1:])
            else:
                continuing = False
        if not lines:
            continue
        path = dest / Path(tst).relative_to(raw).with_suffix('.g')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(''.join(line + '\n' for line in lines), encoding='utf-8')
        out.append(str(path))
    return out


def corpus_list(lang):
    # A grammar parsed over another's ranges reads that one's corpus.
    return CORPUS / RANGES_FROM.get(lang, lang) / 'files.txt'


def git_checkout(spec):
    """'<url>@<commit>': a shallow clone of that one commit under
    build/grammar-corpus/.git/, made once and read in place. Nothing in it
    is run."""
    url, rev = spec.rsplit('@', 1)
    dest = CORPUS / '.git' / url.rstrip('/').split('/')[-1]
    have = subprocess.run(['git', '-C', str(dest), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if have.returncode != 0 or have.stdout.strip() != rev:
        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        run(['git', '-C', str(dest), 'init', '-q'])
        run(['git', '-C', str(dest), 'fetch', '-q', '--depth', '1', url, rev])
        run(['git', '-C', str(dest), '-c', 'advice.detachedHead=false', 'checkout', '-q', 'FETCH_HEAD'])
    return dest


def cmd_corpus(args):
    spec = language(args.lang)
    files = [str(REPO / p) for p in subprocess.run(['git', 'ls-files', *spec['globs'], *[f'*/{g}' for g in spec['globs']]],
                                                   cwd=REPO, check=True, capture_output=True, text=True).stdout.split()]
    print(f'repo: {len(files)} files')
    for attr, patterns in spec['sources']:
        dest = CORPUS / args.lang / attr.replace('@', '_')
        if attr == '@just-tests':
            got = just_test_justfiles(dest)
        elif attr == '@gap-tests':
            got = gap_test_inputs(dest)
        elif attr == '@spec-examples':
            got = spec_examples(dest)
        elif attr.startswith('@zip:'):
            # A zip inside a pinned package ('@zip:<attr>:<member>'): the
            # JDK's src.zip.
            pkg, member = attr[len('@zip:'):].split(':', 1)
            got = unpack(Path(nix_out_path('@pinned:' + pkg)) / member, patterns, dest)
        elif attr.startswith('@git:'):
            got = unpack(git_checkout(attr[len('@git:'):]), patterns, dest)
        else:
            got = unpack(nix_out_path(attr), patterns, dest)
        print(f'{attr}: {len(got)} files')
        files += got
    files = sorted(set(files))
    if spec.get('filter_globs'):
        files = [f for f in files if matches(f, spec['globs'])]
    if spec.get('dedupe'):
        # The same library inside several jars: keep one copy of each text.
        seen, kept = set(), []
        for f in files:
            digest = hashlib.sha256(Path(f).read_bytes()).hexdigest()
            if digest not in seen:
                seen.add(digest)
                kept.append(f)
        print(f'{len(files) - len(kept)} duplicate files dropped')
        files = kept
    out = corpus_list(args.lang)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(''.join(f + '\n' for f in files))
    lines = sum(Path(f).read_bytes().count(b'\n') for f in files)
    print(f'{len(files)} files, {lines} lines -> {out.relative_to(REPO)}')


def read_corpus(args, lang):
    path = Path(args.corpus) if args.corpus else corpus_list(lang)
    if not path.exists():
        die(f'no corpus at {path}; run: just grammar-corpus {lang}')
    listed = [line for line in path.read_text().splitlines() if line]
    files = [f for f in listed if os.path.exists(f)]
    if len(files) < len(listed):
        # A repository file can go away after the list was made (a vendored
        # grammar removed, say); rerun `corpus` to refresh it.
        print(f'corpus: skipping {len(listed) - len(files)} listed files that no longer exist', file=sys.stderr)
    return files


# ---- building the dump tool -------------------------------------------------

def git(*argv):
    return subprocess.run(['git', *argv], cwd=REPO, capture_output=True, text=True)


def old_source(name, override):
    """The replaced grammar's src/ dir, and where it came from."""
    if override:
        return Path(override), override
    vendored = REPO / 'third_party' / 'grammars' / name / 'src'
    if (vendored / 'parser.c').exists():
        return vendored, 'third_party (vendored)'
    if 'old' in LANGUAGES.get(name, {}):
        # A grammar mep loaded from nixpkgs rather than vendoring: its source.
        attr, sub = LANGUAGES[name]['old']
        return Path(nix_out_path(attr)) / sub, f'nixpkgs#{attr}'
    rel = f'third_party/grammars/{name}'
    if git('cat-file', '-e', f'HEAD:{rel}/src/parser.c').returncode == 0:
        rev = git('rev-parse', 'HEAD').stdout.strip()
    else:
        last = git('rev-list', '-1', 'HEAD', '--', rel).stdout.strip()
        if not last:
            die(f'{name} was never vendored; pass --old <src dir> (an upstream checkout, say)')
        rev = git('rev-parse', f'{last}^').stdout.strip()
    dest = BUILD / name / f'old-{rev[:12]}'
    if not (dest / rel / 'src' / 'parser.c').exists():
        dest.mkdir(parents=True, exist_ok=True)
        archive = subprocess.run(['git', 'archive', rev, f'{rel}/src'], cwd=REPO, check=True, capture_output=True)
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(dest, filter='data')
    return dest / rel / 'src', f'git {rev[:12]}'


def new_source(name, override):
    src = Path(override) if override else REPO / 'grammars' / f'tree-sitter-{name}' / 'src'
    if not (src / 'parser.c').exists():
        die(f'no grammar at {src} yet')
    return src, str(src.relative_to(REPO)) if not override else override


# A grammar parsed only over another's ranges: the inline grammar runs
# over the block grammar's `inline` nodes (dump.c's TS_RANGES_LANGUAGE).
# Both variants take their ranges from the same (the replaced) block
# grammar, so a difference is the inline grammar's own.
RANGES_FROM = {'markdown_inline': 'markdown'}


def build_dump(name, src, variant, sanitize=False):
    """build/grammar-diff/<name>/<variant>/dump, rebuilt when its sources change."""
    sources = [src / 'parser.c'] + [p for p in (src / 'scanner.c', src / 'scanner.cc') if p.exists()]
    ranges = []
    if name in RANGES_FROM:
        ranges_src, _ = old_source(RANGES_FROM[name], None)
        ranges = [ranges_src / 'parser.c'] + [p for p in (ranges_src / 'scanner.c',) if p.exists()]
    flags = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else ['-O2']
    key = hashlib.sha256()
    for p in sources + ranges + [TOOL_DIR / 'dump.c', RUNTIME / 'src' / 'lib.c']:
        key.update(p.read_bytes())
    key.update(' '.join(flags).encode())
    out_dir = BUILD / name / variant
    exe, stamp = out_dir / 'dump', out_dir / 'dump.key'
    if exe.exists() and stamp.exists() and stamp.read_text() == key.hexdigest():
        return exe
    out_dir.mkdir(parents=True, exist_ok=True)
    cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
    include = ['-I', str(src), '-I', str(RUNTIME / 'include'), '-I', str(RUNTIME / 'src')]
    objects = []
    for c in sources + [TOOL_DIR / 'dump.c', RUNTIME / 'src' / 'lib.c']:
        obj = out_dir / (c.name + '.o')
        compiler = cxx if c.suffix == '.cc' else cc
        extra = [f'-DTS_LANGUAGE=tree_sitter_{name}'] if c.name == 'dump.c' else []
        if c.name == 'dump.c' and ranges:
            extra.append(f'-DTS_RANGES_LANGUAGE=tree_sitter_{RANGES_FROM[name]}')
        print(f'  building {variant}: {c.name}', file=sys.stderr)
        run([compiler, *flags, '-w', *include, *extra, '-c', str(c), '-o', str(obj)])
        objects.append(str(obj))
    for c in ranges:
        obj = out_dir / ('ranges-' + c.name + '.o')
        print(f'  building {variant}: {RANGES_FROM[name]} {c.name}', file=sys.stderr)
        run([cc, *flags, '-w', '-I', str(c.parent), '-I', str(RUNTIME / 'include'), '-c', str(c), '-o', str(obj)])
        objects.append(str(obj))
    run([cxx, *flags, *objects, '-o', str(exe)])
    stamp.write_text(key.hexdigest())
    return exe


# ---- symbols ----------------------------------------------------------------

def cmd_symbols(args):
    old_src, old_from = old_source(args.name, args.old)
    new_src, new_from = new_source(args.name, args.new)
    old = set(subprocess.run([build_dump(args.name, old_src, 'old'), '--symbols'], check=True, capture_output=True,
                             text=True).stdout.splitlines())
    new = set(subprocess.run([build_dump(args.name, new_src, 'new'), '--symbols'], check=True, capture_output=True,
                             text=True).stdout.splitlines())
    print(f'old: {old_from}: {sum(l.startswith("node") for l in old)} node types, '
          f'{sum(l.startswith("field") for l in old)} fields')
    print(f'new: {new_from}: {sum(l.startswith("node") for l in new)} node types, '
          f'{sum(l.startswith("field") for l in new)} fields')
    for title, items in (('only in old (queries using these would break)', old - new), ('only in new', new - old)):
        print(f'{title}: {len(items)}')
        for item in sorted(items):
            print(f'  {item}')
    return 1 if old - new else 0


# ---- diff -------------------------------------------------------------------

def parse_dump(path):
    trees, errors, current = {}, {}, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('@@FILE '):
                current = line[7:].rstrip('\n')
                trees[current] = []
            elif line.startswith('@@ERRORS '):
                errors[current] = int(line.split()[1])
            else:
                trees[current].append(line.rstrip('\n'))
    return trees, errors


def top_level_items(lines):
    """A dump's top-level items (the root's children) as
    {(start, end): (lines, has_error)}, each with its whole subtree."""
    items, current = {}, None
    for line in lines:
        depth, _, kind, start, end = line.split()
        if depth == '0':
            continue
        if depth == '1':
            current = (int(start), int(end))
            items[current] = ([], False)
        if current is None:
            continue
        sub, err = items[current]
        sub.append(line)
        items[current] = (sub, err or kind in ('ERROR', 'MISSING'))
    return items


def compare_items(old, new, files):
    """Top-level items the old grammar parsed without errors: how many the
    new grammar parses to the identical subtree, and the files of those it
    does not."""
    total = same = 0
    mismatched = collections.Counter()
    clean_but_different = []
    for f in files:
        new_items = top_level_items(new.get(f, []))
        for span, (sub, err) in top_level_items(old[f]).items():
            if err:
                continue
            total += 1
            got = new_items.get(span)
            if got and got[0] == sub:
                same += 1
            else:
                mismatched[f] += 1
                if got and not got[1]:
                    clean_but_different.append((f, span))
    return total, same, mismatched, clean_but_different


def error_bytes(lines):
    """Bytes inside ERROR nodes (outermost only, not counting a root that
    is itself ERROR -- that is the whole file, the same for both grammars)."""
    total, covered_to = 0, -1
    for line in lines:
        depth, _, kind, start, end = line.split()
        start, end = int(start), int(end)
        if kind == 'ERROR' and depth != '0' and start >= covered_to:
            total += end - start
            covered_to = end
    return total


def snippet(path, byte, width=70):
    try:
        data = Path(path).read_bytes()
    except OSError:
        return ''
    start = data.rfind(b'\n', 0, byte) + 1
    end = data.find(b'\n', byte)
    line = data[start:end if end >= 0 else len(data)].decode(errors='replace')
    row = data.count(b'\n', 0, byte) + 1
    return f'line {row}: {line.strip()[:width]}'


def oracle_accepts(oracle, path):
    argv = [path if a == '{}' else a for a in oracle]
    try:
        return subprocess.run(argv, capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def known_differences(name):
    """grammars/tree-sitter-<name>/test/known-differences.txt: reviewed
    differences where the new grammar is right and the old one wrong, as
    `<path glob>  # why`. Blank lines and lines starting with # are skipped."""
    path = REPO / 'grammars' / f'tree-sitter-{name}' / 'test' / 'known-differences.txt'
    if not path.exists():
        return []
    known = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        pattern, _, why = line.partition('#')
        known.append((pattern.strip(), why.strip()))
    return known


def is_known(path, known):
    return next((why for pattern, why in known if fnmatch.fnmatch(path, pattern)), None)


# ---- Markdown's structural oracle -------------------------------------------

# Tree node types that are blocks to cmark, by cmark's names. Sections, link
# reference definitions (cmark drops them) and the rest are left out.
MARKDOWN_BLOCKS = {
    'paragraph': 'paragraph', 'thematic_break': 'thematic_break', 'block_quote': 'block_quote',
    'list': 'list', 'list_item': 'item', 'indented_code_block': 'code_block',
    'fenced_code_block': 'code_block', 'html_block': 'html_block', 'pipe_table': 'table',
}
CMARK_BLOCKS = {'paragraph', 'thematic_break', 'block_quote', 'list', 'item', 'code_block', 'html_block',
                'table', 'heading'}


def markdown_skeleton(lines):
    """A tree's blocks as (depth, kind) in document order, depth counting
    only block quotes, lists and items -- the shape cmark's XML has."""
    out, stack = [], []  # stack: (tree depth, is container)
    heading_depth = None
    for line in lines:
        d, _, t = line.split()[:3]
        d = int(d)
        while stack and stack[-1][0] >= d:
            stack.pop()
        if heading_depth is not None and d <= heading_depth:
            heading_depth = None
        nest = sum(1 for _, c in stack if c)
        if t in ('atx_heading', 'setext_heading'):
            out.append([nest, 'heading'])
            heading_depth = d
            stack.append((d, False))
            continue
        if heading_depth is not None:
            # The level, from the marker or underline; nothing else inside.
            for level in '123456':
                if t in (f'atx_h{level}_marker', f'setext_h{level}_underline'):
                    out[-1][1] = f'heading{level}'
            continue
        kind = MARKDOWN_BLOCKS.get(t)
        if kind:
            out.append([nest, kind])
        stack.append((d, kind in ('block_quote', 'list', 'item')))
    return [tuple(x) for x in out]


def cmark_skeleton(xml_text):
    import xml.etree.ElementTree as ET
    root = ET.fromstring(xml_text)
    out = []
    def walk(el, nest):
        tag = el.tag.split('}')[-1]
        if tag in CMARK_BLOCKS:
            out.append((nest, f'heading{el.get("level")}' if tag == 'heading' else tag))
            if tag in ('block_quote', 'list', 'item'):
                nest += 1
            elif tag in ('paragraph', 'heading', 'code_block', 'html_block', 'table', 'thematic_break'):
                return
        for child in el:
            walk(child, nest)
    walk(root, 0)
    return out


def cmark_binaries():
    """cmark-gfm (GFM's tables and task lists, on CommonMark 0.29) and cmark
    (CommonMark 0.31): a tree agreeing with either reads the file right."""
    return [[str(Path(nix_out_path('cmark-gfm')) / 'bin' / 'cmark-gfm'), '-e', 'table', '-e', 'tasklist'],
            [str(Path(nix_out_path('cmark')) / 'bin' / 'cmark')]]


def markdown_oracle(differ, old, new):
    """For each differing file: does cmark-gfm's block structure agree with
    the new tree, the old one, or neither? Fails when only the old agrees."""
    binaries = cmark_binaries()
    def judge(f):
        if any(' minus_metadata ' in l or ' plus_metadata ' in l for l in new.get(f, [])):
            return 'front matter (cmark has none)'
        wants = [cmark_skeleton(subprocess.run([*b, '--to', 'xml', f], capture_output=True, text=True,
                                               errors='replace').stdout) for b in binaries]
        n, o = markdown_skeleton(new.get(f, [])), markdown_skeleton(old.get(f, []))
        if n in wants:
            return 'new agrees with cmark'
        if o in wants:
            return 'only old agrees with cmark'
        return 'neither agrees with cmark'
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        verdicts = dict(zip(differ, pool.map(judge, differ)))
    counts = collections.Counter(verdicts.values())
    print(f'\noracle (cmark-gfm/cmark block structure), {len(differ)} differing files:')
    for verdict, count in counts.most_common():
        print(f'  {count:5}  {verdict}')
    bad = [f for f, v in verdicts.items() if v == 'only old agrees with cmark']
    for f in bad:
        print(f'  fails: {f}')
    for f, v in verdicts.items():
        if v == 'neither agrees with cmark':
            print(f'  neither: {f}')
    return 1 if bad else 0


def cmd_diff(args):
    spec = language(args.name)
    files = read_corpus(args, args.name)
    old_src, old_from = old_source(args.name, args.old)
    new_src, new_from = new_source(args.name, args.new)
    dumps = {}
    for variant, src in (('old', old_src), ('new', new_src)):
        exe = build_dump(args.name, src, variant)
        out = BUILD / args.name / variant / 'trees.txt'
        with open(out, 'w') as f:
            run([str(exe)], input=''.join(p + '\n' for p in files), stdout=f, text=True)
        dumps[variant] = parse_dump(out)
    (old, old_err), (new, new_err) = dumps['old'], dumps['new']

    all_differ = [f for f in old if old[f] != new.get(f)]
    known = known_differences(args.name)
    expected = {f: is_known(f, known) for f in all_differ if is_known(f, known)}
    differ = [f for f in all_differ if f not in expected]
    print(f'old: {old_from}\nnew: {new_from}')
    print(f'{len(old)} files: {len(old) - len(all_differ)} identical, {len(all_differ)} differ'
          + (f' ({len(expected)} of them known differences, {len(differ)} not)' if known else ''))
    errs_old = {f for f, n in old_err.items() if n}
    errs_new = {f for f, n in new_err.items() if n}
    print(f'files with parse errors: old {len(errs_old)}, new {len(errs_new)} '
          f'({len(errs_new - errs_old)} only in new, {len(errs_old - errs_new)} only in old)')

    buckets, examples = collections.Counter(), {}
    for f in differ:
        a, b = old[f], new.get(f, [])
        i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        left = a[i].split() if i < len(a) else ['', 'END', '', '0', '0']
        right = b[i].split() if i < len(b) else ['', 'END', '', '0', '0']
        key = f'{left[1]} {left[2]}  ->  {right[1]} {right[2]}'
        buckets[key] += 1
        examples.setdefault(key, (f, left, right))
    if buckets:
        print('\nfirst difference in each file, by kind (old -> new):')
    for key, count in buckets.most_common(args.kinds):
        f, left, right = examples[key]
        print(f'{count:6}  {key}')
        print(f'        e.g. {os.path.relpath(f, CORPUS) if f.startswith(str(CORPUS)) else f}')
        print(f'             old {left[3:]} new {right[3:]}  {snippet(f, int(left[3] or 0))}')

    if expected and args.show_known:
        reasons = collections.Counter(expected.values())
        print('\nknown differences (test/known-differences.txt):')
        for why, count in reasons.most_common():
            print(f'{count:6}  {why}')
    unused = [p for p, _ in known if not any(fnmatch.fnmatch(f, p) for f in all_differ)]
    if unused:
        print(f'\nknown-differences entries that no longer match a differing file: {len(unused)}')
        for p in unused:
            print(f'  {p}')

    status = 0
    if args.oracle and spec.get('structure_oracle') == 'markdown' and args.name == 'markdown':
        status = markdown_oracle(differ, old, new)
    elif args.oracle and spec.get('structure_oracle'):
        print('\noracle: none for the inline grammar (its differences are reviewed by hand)')
        status = 1 if differ else 0
    elif args.oracle:
        if not spec['oracle']:
            die(f'{args.name} has no oracle (see LANGUAGES in this script)')
        suspects = sorted(set(differ) | {f for f in errs_new - errs_old if f not in expected})
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
            accepted = [f for f, ok in zip(suspects, pool.map(lambda f: oracle_accepts(spec['oracle'], f), suspects))
                        if ok]
        print(f'\noracle ({spec["oracle"][0]}): of {len(suspects)} files that differ or fail only in new, '
              f'{len(suspects) - len(accepted)} are rejected by the language itself, {len(accepted)} accepted')
        for f in accepted:
            print(f'  accepted: {f}' + ('  [parse error in new]' if f in errs_new else ''))
        status = 1 if accepted else 0
    elif args.old_clean:
        clean = [f for f in differ if f not in errs_old]
        total, same, mismatched, clean_but_different = compare_items(old, new, differ)
        total_all = total + sum(1 for f in old if f not in differ for _, (_, e) in top_level_items(old[f]).items()
                                if not e)
        print(f'\nold-clean: {len(clean)} differing files the old grammar parsed without errors')
        for f in clean[:args.kinds]:
            print(f'  {f}')
        print(f'top-level items the old grammar parsed without errors: {total_all}; '
              f'{total_all - (total - same)} identical in the new, {total - same} not, in {len(mismatched)} files')
        clean_but_different = [(f, span) for f, span in clean_but_different if f not in expected]
        print(f'  parsed without errors by both but differently: {len(clean_but_different)} (fails)')
        for f, (start, end) in clean_but_different[:args.kinds]:
            print(f'    {f}:{start}-{end}  {snippet(f, start)}')
        recovery = (total - same) - len(clean_but_different)
        print(f'  inside or across an error in the new tree: {recovery} (error recovery; see below)')
        for f, n in mismatched.most_common(args.kinds):
            print(f'    {n:4}  {f}')
        old_err = sum(error_bytes(old[f]) for f in old)
        new_err = sum(error_bytes(new[f]) for f in new)
        worse = sum(1 for f in old if error_bytes(new[f]) > error_bytes(old[f]))
        better = sum(1 for f in old if error_bytes(new[f]) < error_bytes(old[f]))
        print(f'bytes inside ERROR nodes: old {old_err}, new {new_err} '
              f'({100 * (new_err - old_err) / max(old_err, 1):+.1f}%); new worse in {worse} files, better in {better}')
        status = 1 if [f for f in clean if f not in expected] or clean_but_different else 0
    elif differ:
        status = 1
    return status


# ---- fuzz -------------------------------------------------------------------

def mutate(data, tokens, rng):
    b = bytearray(data)
    for _ in range(rng.randint(1, 8)):
        pos, op = rng.randrange(len(b) + 1), rng.random()
        if op < 0.4 and b:
            del b[pos:pos + rng.randint(1, 20)]
        elif op < 0.8:
            b[pos:pos] = rng.choice(tokens).encode()
        elif b:
            b[min(pos, len(b) - 1)] = rng.randrange(256)
    return bytes(b)


def make_mutants(args, spec, files, dirname):
    """args.count damaged copies of corpus files (seeded), in build/grammar-diff/<name>/<dirname>
    (one per command, so a fuzz and a recovery run can share a machine)."""
    mutants_dir = BUILD / args.name / dirname
    if mutants_dir.exists():
        shutil.rmtree(mutants_dir)
    mutants_dir.mkdir(parents=True)
    rng = random.Random(args.seed)
    tokens = GENERIC_TOKENS + spec['tokens']
    mutants = []
    for i in range(args.count):
        data = Path(rng.choice(files)).read_bytes()
        path = mutants_dir / f'{i:05d}{Path(files[0]).suffix or ".txt"}'
        path.write_bytes(mutate(data, tokens, rng))
        mutants.append(str(path))
    return mutants


def cmd_fuzz(args):
    spec = language(args.name)
    files = read_corpus(args, args.name)
    src, src_from = (old_source(args.name, args.old) if args.fuzz_old else new_source(args.name, args.new))
    exe = build_dump(args.name, src, 'old-asan' if args.fuzz_old else 'asan', sanitize=True)

    mutants = make_mutants(args, spec, files, 'mutants')
    queue = ([] if args.mutants_only else files) + mutants
    print(f'{src_from}: {len(queue)} files ({len(mutants)} damaged, seed {args.seed}) under ASan + UBSan')

    env = dict(os.environ, DUMP_TRACE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
               UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1')
    env.pop('LD_LIBRARY_PATH', None)
    if shutil.which('llvm-symbolizer'):
        env['ASAN_SYMBOLIZER_PATH'] = shutil.which('llvm-symbolizer')
    failures_dir = BUILD / args.name / 'failures'
    if failures_dir.exists():
        shutil.rmtree(failures_dir)
    failures = []
    batch = 500
    i = 0
    while i < len(queue):
        chunk = queue[i:i + batch]
        log = BUILD / args.name / 'fuzz.log'
        with open(log, 'w') as err:
            try:
                proc = subprocess.run([str(exe)], input=''.join(p + '\n' for p in chunk), stdout=subprocess.DEVNULL,
                                      stderr=err, text=True, env=env, timeout=args.timeout)
                failed = proc.returncode != 0
                kind = 'crash'
            except subprocess.TimeoutExpired:
                failed, kind = True, 'hang'
        if not failed:
            i += len(chunk)
            continue
        text = log.read_text(errors='replace')
        parsing = re.findall(r'^@@PARSING (.*)$', text, re.M)
        culprit = parsing[-1] if parsing else chunk[0]
        report = text[text.rfind('@@PARSING'):].split('\n', 1)[-1].strip()
        failures_dir.mkdir(parents=True, exist_ok=True)
        saved = failures_dir / f'{len(failures):03d}-{kind}{Path(culprit).suffix}'
        shutil.copyfile(culprit, saved)
        failures.append((kind, culprit, saved, report))
        print(f'{kind}: {culprit} (saved as {saved.relative_to(REPO)})')
        for line in report.splitlines()[:8]:
            print(f'    {line}')
        i += chunk.index(culprit) + 1 if culprit in chunk else len(chunk)
    print(f'{len(failures)} crash(es)/hang(s)' if failures else 'clean')
    return 1 if failures else 0


# ---- recovery ---------------------------------------------------------------

def stream_error_bytes(exe, files):
    """error_bytes of each file's tree, read from the dump as it runs: the
    trees of a few thousand damaged files run to gigabytes."""
    out, current, lines = {}, None, []
    with subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                          errors='replace') as proc:
        # Fed from a thread: the dump answers each name as it reads it, so
        # writing the whole list first deadlocks once both pipes fill.
        def feed():
            proc.stdin.write(''.join(f + '\n' for f in files))
            proc.stdin.close()
        feeder = threading.Thread(target=feed)
        feeder.start()
        for line in proc.stdout:
            if line.startswith('@@FILE '):
                current, lines = line[7:].rstrip('\n'), []
            elif line.startswith('@@ERRORS '):
                out[current] = error_bytes(lines)
            else:
                lines.append(line)
    feeder.join()
    if proc.returncode != 0:
        die(f'{exe} failed (exit {proc.returncode}) after {len(out)} of {len(files)} files')
    return out


def cmd_recovery(args):
    """How much of each damaged file ends up inside ERROR nodes, old grammar
    vs new: code being typed is broken most of the time, and an editor
    colours what recovery leaves outside the errors. Informational -- the
    two recover differently file by file; the totals are the measure."""
    spec = language(args.name)
    mutants = make_mutants(args, spec, read_corpus(args, args.name), 'recovery-mutants')
    totals, per_file = {}, {}
    for variant, (src, _) in (('old', old_source(args.name, args.old)), ('new', new_source(args.name, args.new))):
        per_file[variant] = stream_error_bytes(build_dump(args.name, src, variant), mutants)
        totals[variant] = sum(per_file[variant].values())
    old, new = per_file['old'], per_file['new']
    better = sum(new[m] < old[m] for m in mutants)
    worse = sorted(((new[m] - old[m], m) for m in mutants if new[m] > old[m]), reverse=True)
    change = (totals['new'] - totals['old']) / max(totals['old'], 1)
    print(f'{len(mutants)} damaged files (seed {args.seed}): bytes in ERROR nodes old {totals["old"]}, '
          f'new {totals["new"]} ({change:+.1%}); new better in {better}, worse in {len(worse)}')
    for delta, m in worse[:args.worst]:
        print(f'  +{delta}  {m}')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('corpus', help='fetch and unpack a corpus')
    p.add_argument('lang')
    p.set_defaults(func=cmd_corpus)

    for name, func, helptext in (('symbols', cmd_symbols, 'compare node types and fields'),
                                 ('diff', cmd_diff, 'compare trees over the corpus'),
                                 ('fuzz', cmd_fuzz, 'damaged files under ASan + UBSan'),
                                 ('recovery', cmd_recovery, 'error recovery on damaged files, old vs new')):
        p = sub.add_parser(name, help=helptext)
        p.add_argument('name', help='grammar name')
        p.add_argument('--old', help='src dir of the grammar being replaced')
        p.add_argument('--new', help='src dir of the new grammar')
        p.set_defaults(func=func)
        if name != 'symbols':
            p.add_argument('--corpus', help='file list (default: build/grammar-corpus/<name>/files.txt)')
    sub.choices['diff'].add_argument('--oracle', action='store_true',
                                     help="ask the language's own implementation about each differing file")
    sub.choices['diff'].add_argument('--kinds', type=int, default=15, help='difference kinds to show')
    sub.choices['diff'].add_argument('--old-clean', action='store_true',
                                     help='for a language with no oracle: fail only on differing files '
                                          'the old grammar parsed without errors')
    sub.choices['diff'].add_argument('--show-known', action='store_true',
                                     help='list the known differences by reason')
    for name in ('fuzz', 'recovery'):
        sub.choices[name].add_argument('--count', type=int, default=3000, help='damaged files to make')
        sub.choices[name].add_argument('--seed', type=int, default=7)
    sub.choices['recovery'].add_argument('--worst', type=int, default=10, help='worst files to list')
    fuzz = sub.choices['fuzz']
    fuzz.add_argument('--timeout', type=int, default=300, help='seconds per batch of 500 before calling it a hang')
    fuzz.add_argument('--mutants-only', action='store_true', help='skip the undamaged corpus')
    fuzz.add_argument('--fuzz-old', action='store_true', help='fuzz the old grammar instead')

    args = parser.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == '__main__':
    main()
