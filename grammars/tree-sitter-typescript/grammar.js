/**
 * @file Tree-sitter grammar for TypeScript (.ts, .mts, .cts).
 *
 * The rules are shared with the TSX dialect (../tree-sitter-tsx): see
 * common/define-grammar.js.
 */

module.exports = require('./common/define-grammar.js')('typescript');
