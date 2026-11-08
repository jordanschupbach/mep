#pragma once

// mep-zig-lsp's semantic checks: the part of Zig's own AstGen
// (lib/std/zig/AstGen.zig, MIT, Copyright (c) Zig contributors) that
// `zig ast-check` reports without types -- scopes and names (undeclared
// identifiers, unused locals/parameters/captures, `var` never mutated,
// pointless discards, shadowing, redeclaration), labels, container member
// names, builtin names and arities, number/string/char literal checks,
// `return`/`try`/`break`/`continue` placement, unreachable code -- ported
// decision for decision so the messages, positions and the per-declaration
// "stop at the first hard error" behaviour are AstGen's own.
//
// It also records what the language server needs from the same walk: which
// declaration every identifier resolves to, and the scopes with the names
// visible in each (for completion).
//
// Pure: no I/O; works on a tree from zig::parse (zig_ast.h). Only run it
// on a tree without parse errors (AstGen does not run otherwise).

#include <cstdint>
#include <string>
#include <vector>

#include "zig_ast.h"

namespace zigcheck {

struct Note {
    std::uint32_t byte_offset = 0;  // into the source
    std::string message;
};

struct Diagnostic {
    std::uint32_t byte_offset = 0;      // where Zig puts the caret
    std::uint32_t end_offset = 0;       // end of the token/node it is about (>= byte_offset)
    std::string message;                // exactly AstGen's text
    std::string code;                   // stable id: unused-local, undeclared-identifier, ...
    std::vector<Note> notes;
};

enum class BindingKind {
    container_const,  // `const x = ...;` in a container (file or struct/union/enum/opaque)
    container_var,
    function,         // container-level `fn`
    test,             // named test (`test name {}` with an identifier)
    field,            // container field (not in scope by itself; for symbols/navigation)
    local_const,
    local_var,
    param,
    capture,          // if/while/for/switch/catch/errdefer payloads
    label,            // block/loop/switch labels
};

struct Binding {
    zig::TokenIndex name_token = 0;  // the declaring identifier
    BindingKind kind = BindingKind::local_const;
    zig::NodeIndex node = 0;         // the declaration's node (0 when none, e.g. a capture)
    int scope = -1;                  // index into Result::scopes where it is declared
};

struct Scope {
    std::uint32_t start = 0, end = 0;  // byte range where the scope's names are visible
    int parent = -1;
    bool is_container = false;          // a namespace: names visible in the whole range, order-free
    zig::NodeIndex node = 0;            // the container / block / fn node
    std::vector<int> bindings;          // indices into Result::bindings
};

struct Result {
    std::vector<Diagnostic> diagnostics;
    std::vector<Binding> bindings;
    std::vector<Scope> scopes;
    // For every identifier token that resolves: (use token, binding index).
    // Declaring tokens are not listed here. Field accesses (`a.b`) and enum
    // literals are not resolved (that needs types).
    std::vector<std::pair<zig::TokenIndex, int>> references;
};

/** @brief Runs the checks over a parsed tree (no parse errors). */
Result Check(const zig::Ast &tree);

}  // namespace zigcheck
