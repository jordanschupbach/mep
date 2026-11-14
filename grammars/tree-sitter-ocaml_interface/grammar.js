/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const ocaml = require('../tree-sitter-ocaml/grammar.js');

module.exports = grammar(ocaml, {
  name: 'ocaml_interface',

  rules: {
    compilation_unit: $ => optional($._signature),
  },
});
