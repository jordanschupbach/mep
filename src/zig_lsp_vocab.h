#pragma once

// mep-zig-lsp's vocabulary, generated from Zig's own source tree by
// tools/gen_zig_vocab.py into zig_lsp_vocab_data.cpp (`just zig-lsp-vocab`):
//
//   kVocab     every builtin (`@name`, from doc/langref.html.in) and the public
//              declarations of lib/std (`std.mem.eql`, `std.ArrayList.append`)
//              with a one-line signature and the doc text (MIT, Zig
//              contributors; the notice is in the generated file).
//   kBuiltins  lib/std/zig/BuiltinFn.zig's table: parameter count, whether
//              the builtin may only appear in a function, whether it can be
//              assigned to -- what AstGen checks (zig_check.cpp).

#include <cstddef>
#include <string_view>

namespace zig_lsp {

struct VocabEntry {
    const char *name;  // "@memcpy", "std.mem.eql", "std.ArrayList.append"
    const char *kind;  // builtin | fn | type | const | var
    const char *sig;   // one line
    const char *doc;   // may be empty
};

extern const VocabEntry kVocab[];
extern const std::size_t kVocabCount;

struct BuiltinInfo {
    const char *name;               // "@addWithOverflow"
    int param_count;                // -1: variable
    bool illegal_outside_function;
    bool allows_lvalue;
};

extern const BuiltinInfo kBuiltins[];
extern const std::size_t kBuiltinCount;

/** @brief The BuiltinFn.zig entry for `name` ("@memcpy"), or nullptr. */
const BuiltinInfo *findBuiltin(std::string_view name);

/** @brief The kVocab entry named exactly `name`, or nullptr. */
const VocabEntry *findVocab(std::string_view name);

}  // namespace zig_lsp
