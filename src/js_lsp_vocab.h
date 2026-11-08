#pragma once

// The names mep-js-lsp knows: JavaScript's globals and their members, with
// TypeScript's signatures (and, for the ECMAScript library, its JSDoc), the
// DOM's names and signatures, and Node.js's globals. Generated into
// js_lsp_vocab.cpp by tools/gen_js_vocab.js.

#include <cstddef>

struct JsLspApi {
    const char *origin;     // "es", "dom", "node"
    const char *name;       // "parseInt", "Math.max", "Array.prototype.map", "HTMLElement"
    const char *kind;       // function, method, property, variable, class, interface, type, namespace, ...
    const char *signature;  // TypeScript's: "(x: number, ...values: number[]): number" -- overloads one per line
    const char *doc;        // markdown ("" for DOM names)
};

const JsLspApi *JsLspApiTable(size_t *count);
