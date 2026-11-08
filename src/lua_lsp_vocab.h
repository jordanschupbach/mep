#pragma once

// The names mep-lua-lsp knows: Lua 5.4's standard library (with the
// reference manual's signatures and text), names from other Lua versions,
// and mep's own `mep.*` API. Generated into lua_lsp_vocab.cpp by
// tools/gen_lua_vocab.py.

#include <cstddef>

struct LuaLspApi {
    const char *source;     // "std", "old" (another Lua version), "mep"
    const char *name;       // "print", "string.format", "file:read", "mep.get_line"
    const char *signature;  // "(formatstring, ...)" -- "" for a value
    const char *kind;       // "function", "table", "string", "number", "value"
    const char *doc;        // markdown
};

const LuaLspApi *LuaLspApiTable(size_t *count);
