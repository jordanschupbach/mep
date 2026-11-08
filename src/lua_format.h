#pragma once

// mep's own Lua formatter: the builtin behind `gf` in a Lua buffer
// (mep.format_lua), and textDocument/formatting in mep-lua-lsp.
// A port of StyLua 2.3.1's layout rules (lua_format.cpp), checked byte for
// byte against `stylua -` with `just format-compare lua`.

#include <string>
#include <string_view>

namespace luafmt {

// StyLua 2.3.1's defaults: column_width 120, tabs (a tab counts as `indent`
// columns), indent_width 4, AutoPreferDouble quotes, call parentheses always,
// no simple-statement collapsing, Unix line endings.
struct Options {
    int width = 120;
    int indent = 4;
    bool use_tabs = true;
    // Re-lex the output and refuse (returning the input) if the program or its comments changed.
    bool verify = true;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Lua source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace luafmt
