#pragma once

// mep's JavaScript formatter: a C++ port of Prettier 3.6.2 (MIT, James Long
// and contributors) over mep's own acorn port (js_parse.h). Same comment
// attachment, same estree printer, same Wadler-style doc printer, so the
// output is meant to be byte-identical to `prettier --parser acorn` (and
// `prettier --stdin-filepath x.js`, which uses babel and almost always
// agrees) -- `just format-compare javascript` measures it over a corpus.
//
// TypeScript (Options::typescript, with jsx for .tsx): js_parse's
// TypeScript dialect gives typescript-estree's trees, and the printer has
// Prettier's TypeScript printers (print/typescript.js, type-annotation.js,
// interface, enum, mapped types, casts, decorators), so the output matches
// `prettier --parser typescript` -- `just format-compare typescript` / `tsx`.
//
// What is not ported: embedded languages in template literals (CSS in
// styled-components, GraphQL, HTML, Markdown are left as written, where
// Prettier would reformat them) and Flow syntax.
//
// Safety: after printing, the output is parsed again and its AST compared
// with the input's (ignoring positions and what Prettier legitimately
// changes: parentheses, quotes, number spelling, JSX whitespace), and every
// comment must survive; on any mismatch the input is returned with
// ok=false. Nesting deeper than Options::max_depth and inputs that exhaust
// the work budget are refused the same way, never crashed on.

#include <string>
#include <string_view>

namespace jsfmt {

// Prettier's defaults.
struct Options {
    int width = 80;            // printWidth
    int indent = 2;            // tabWidth
    bool use_tabs = false;     // useTabs
    bool semi = true;          // semi
    bool single_quote = false; // singleQuote
    bool jsx_single_quote = false;
    bool bracket_spacing = true;
    bool bracket_same_line = false;
    bool arrow_parens_always = true;  // arrowParens "always" ("avoid" otherwise)
    bool trailing_comma_all = true;   // trailingComma "all" ("es5" otherwise)
    bool jsx = true;                  // parse JSX (with typescript: a .tsx file)
    bool typescript = false;          // the TypeScript dialect (prettier --parser typescript)
    bool dts = false;                 // with typescript: a declaration file (.d.ts)
    bool verify = true;               // re-parse and compare (refuse on mismatch)
    int max_depth = 500;              // AST nesting refused beyond this
};

struct Result {
    bool ok = false;
    std::string text;   // the formatted text, or the input unchanged when !ok
    std::string error;
    int error_line = 0; // 1-based; 0 when not tied to a line
};

/** @brief Sets typescript/jsx/dts from a file name: .ts .mts .cts (.d.ts: dts) are TypeScript, .tsx is
 *  TypeScript + JSX, anything else (.js .jsx .mjs .cjs) is JavaScript + JSX. */
void SetDialectForPath(Options& opts, std::string_view path);

/** @brief Formats JavaScript/JSX (or, with Options::typescript, TypeScript/TSX) source the way Prettier 3.6.2 does; on failure `ok` is false and `text` is the input. */
Result Format(std::string_view src, const Options& opts = {});

}  // namespace jsfmt
