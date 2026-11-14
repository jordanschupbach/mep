/**
 * tree-sitter-apl: mep's own grammar for APL source, GNU APL and Dyalog APL.
 *
 * APL can't be fully parsed without knowing which names are functions, so
 * expressions are flat sequences of atoms (numbers, strings, names, glyphs,
 * parentheses, brackets, dfns). The structure that matters to an editor is
 * real nodes: dfns and their guards, tradfns (`∇ header ... ∇`) with their
 * header parts and locals, Dyalog control structures, :Namespace/:Class
 * scripts, labels, assignments and system commands.
 *
 * Statements end at a newline or a diamond; `∇` at the start of a statement
 * outside a dfn opens or closes a tradfn, inside a dfn it is self-reference.
 */

// Case-insensitive control word, e.g. kw($, 'EndIf') matches :EndIf, :endif, :ENDIF.
function kw($, word) {
  let re = ':';
  for (const ch of word) {
    const lo = ch.toLowerCase(), up = ch.toUpperCase();
    re += lo === up ? ch : '[' + lo + up + ']';
  }
  return alias(token(new RegExp(re)), $.keyword);
}

const NAME = /[A-Za-z_∆⍙][A-Za-z_∆⍙¯0-9]*/;

module.exports = grammar({
  name: 'apl',

  extras: $ => [/[ \t\r\f]/, $.comment],

  conflicts: $ => [[$.name_list, $.operator_spec]],

  inline: $ => [$._header_core, $._header_name, $._arg_spec, $._result_spec],

  rules: {
    source_file: $ => seq(
      repeat(choice($._sep, seq($._item, $._sep))),
      optional($._item),
    ),

    _sep: $ => choice($._newline, $.diamond),
    _newline: _ => /\n/,
    diamond: _ => choice('⋄', '◊'),  // ◊: GNU APL

    // Everything a script or namespace body holds.
    _item: $ => choice(
      $._statement,
      $.tradfn,
      $.namespace,
      $.class_definition,
      $.property,
      $.system_command,
    ),

    // Everything a tradfn or control-structure body holds.
    _statement: $ => choice(
      $._top_expression,
      $.label,
      seq($.label, $._top_expression),
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.repeat_statement,
      $.select_statement,
      $.trap_statement,
      $.with_statement,
      $.hold_statement,
      $.control_statement,
    ),

    system_command: _ => token(choice(
      /\)[A-Za-z][^\n]*/,
      /\][A-Za-z][^\n]*/,
    )),

    label: $ => seq(field('name', $.identifier), ':'),

    // --- tradfns ------------------------------------------------------------------

    tradfn: $ => seq(
      '∇',
      field('header', $.tradfn_header),
      $._newline,
      repeat(choice($._sep, seq($._statement, $._sep))),
      '∇',
      optional('⍫'),
    ),

    tradfn_header: $ => seq(
      optional(seq(field('result', $._result_spec), '←')),
      $._header_core,
      repeat(field('local', $.local)),
    ),

    _result_spec: $ => choice(
      $.identifier,
      $.optional_name,
      $.name_list,
    ),

    _header_core: $ => choice(
      field('name', $._header_name),
      seq(field('name', $._header_name), field('right', $._arg_spec)),
      seq(field('left', $._arg_spec), field('name', $._header_name), field('right', $._arg_spec)),
    ),

    _header_name: $ => choice($.identifier, $.operator_spec),
    _arg_spec: $ => choice($.identifier, $.optional_name),

    optional_name: $ => seq('{', $.identifier, '}'),
    name_list: $ => seq('(', repeat1($.identifier), ')'),

    operator_spec: $ => seq(
      '(',
      field('left_operand', $.identifier),
      field('operator', $.identifier),
      optional(field('right_operand', $.identifier)),
      ')',
    ),

    local: $ => seq(';', choice($.identifier, $.system_name)),

    // --- Dyalog control structures ----------------------------------------------

    _body: $ => seq($._sep, repeat(choice($._sep, seq($._statement, $._sep)))),

    if_statement: $ => seq(
      kw($, 'If'), field('condition', $._expression),
      $._body,
      repeat(choice($.elseif_clause, $.andif_clause)),
      optional($.else_clause),
      choice(kw($, 'EndIf'), kw($, 'End')),
    ),
    elseif_clause: $ => seq(kw($, 'ElseIf'), field('condition', $._expression), $._body),
    andif_clause: $ => seq(choice(kw($, 'AndIf'), kw($, 'OrIf')), field('condition', $._expression), $._body),
    else_clause: $ => seq(kw($, 'Else'), $._body),

    for_statement: $ => seq(
      kw($, 'For'), field('variable', $._atoms),
      choice(kw($, 'In'), kw($, 'InEach')), field('iterable', $._expression),
      $._body,
      choice(kw($, 'EndFor'), kw($, 'End')),
    ),

    while_statement: $ => seq(
      kw($, 'While'), field('condition', $._expression),
      $._body,
      choice(kw($, 'EndWhile'), kw($, 'End'), seq(kw($, 'Until'), field('until', $._expression))),
    ),

    repeat_statement: $ => seq(
      kw($, 'Repeat'),
      $._body,
      choice(kw($, 'EndRepeat'), kw($, 'End'), seq(kw($, 'Until'), field('until', $._expression))),
    ),

    select_statement: $ => seq(
      kw($, 'Select'), field('value', $._expression),
      $._body,
      repeat($.case_clause),
      optional($.else_clause),
      choice(kw($, 'EndSelect'), kw($, 'End')),
    ),
    case_clause: $ => seq(choice(kw($, 'Case'), kw($, 'CaseList')), field('value', $._expression), $._body),

    trap_statement: $ => seq(
      kw($, 'Trap'), field('value', $._expression),
      $._body,
      repeat($.case_clause),
      optional($.else_clause),
      choice(kw($, 'EndTrap'), kw($, 'End')),
    ),

    with_statement: $ => seq(
      kw($, 'With'), field('value', $._expression),
      $._body,
      choice(kw($, 'EndWith'), kw($, 'End')),
    ),

    hold_statement: $ => seq(
      kw($, 'Hold'), field('value', $._expression),
      $._body,
      choice(kw($, 'EndHold'), kw($, 'End')),
    ),

    // --- Namespace scripts --------------------------------------------------------

    _items_body: $ => seq($._sep, repeat(choice($._sep, seq($._item, $._sep)))),

    namespace: $ => seq(
      kw($, 'Namespace'), optional(field('name', choice($.identifier, $.qualified_name))),
      $._items_body,
      choice(kw($, 'EndNamespace'), kw($, 'End')),
    ),

    class_definition: $ => seq(
      choice(kw($, 'Class'), kw($, 'Interface')),
      field('name', $.identifier),
      optional(seq(':', field('base', $._atoms))),
      $._items_body,
      choice(kw($, 'EndClass'), kw($, 'EndInterface'), kw($, 'End')),
    ),

    property: $ => seq(
      kw($, 'Property'), optional($._atoms),
      $._items_body,
      choice(kw($, 'EndProperty'), kw($, 'End')),
    ),

    // --- expressions ----------------------------------------------------------------

    // A statement can't start with ∇ outside a dfn: that opens a tradfn.
    _top_expression: $ => choice(
      alias($.top_assignment, $.assignment),
      $._top_atoms,
    ),
    top_assignment: $ => prec.right(seq(
      field('target', $._top_atoms), '←', field('value', $._expression),
    )),
    _top_atoms: $ => seq($._first_atom, repeat($._atom)),

    _expression: $ => choice($.assignment, $._atoms),
    assignment: $ => prec.right(seq(
      field('target', $._atoms), '←', field('value', $._expression),
    )),
    _atoms: $ => repeat1($._atom),

    _atom: $ => choice($._first_atom, $.self_reference),

    _first_atom: $ => choice(
      $.number,
      $.string,
      $.identifier,
      $.qualified_name,
      $.system_name,
      $.zilde,
      $.special_name,
      $.primitive,
      $.operator,
      $.branch,
      $.parenthesized,
      $.index,
      $.dfn,
    ),

    parenthesized: $ => seq('(', optional($._expression), ')'),

    // Indexing x[i;j] and axis f[k]: one optional expression per ;-separated slot.
    index: $ => seq(
      '[',
      optional($._expression),
      repeat(seq(';', optional($._expression))),
      ']',
    ),

    // --- dfns --------------------------------------------------------------------------

    dfn: $ => seq(
      '{',
      repeat(choice($._sep, seq($._dfn_statement, $._sep))),
      optional($._dfn_statement),
      '}',
    ),

    _dfn_statement: $ => choice($._expression, $.guard, $.error_guard),

    guard: $ => seq(
      field('condition', $._expression), ':',
      optional(field('value', $._expression)),
    ),

    error_guard: $ => seq(
      field('codes', $._expression), '::',
      optional(field('value', $._expression)),
    ),

    // ⍺ ⍵ ⍺⍺ ⍵⍵ (and GNU APL's lambda names λ χ).
    special_name: _ => choice('⍺⍺', '⍵⍵', '⍺', '⍵', 'λ', 'χ'),
    // ∇ and ∇∇: the dfn (or dop) itself.
    self_reference: _ => choice('∇∇', '∇'),

    // --- tokens --------------------------------------------------------------------------

    branch: _ => '→',

    primitive: _ => choice(
      '+', '-', '−', '×', '÷', '⌈', '⌊', '*', '⍟', '|', '!', '○', '~', '∼', '∨', '∧', '⍱', '⍲',
      '<', '≤', '=', '≥', '>', '≠', '≡', '≢', '⍴', ',', '⍪', '⌽', '⊖', '⍉', '↑', '↓', '⊂', '⊃',
      '⊆', '⊇', '⌷', '⍋', '⍒', '⍳', '⍸', '∊', '⍷', '∪', '∩', '⊣', '⊢', '⍎', '⍕', '⌹', '?',
      '⊥', '⊤', '⍞', '⍯',
    ),

    operator: _ => choice(
      '/', '\\', '⌿', '⍀', '¨', '⍨', '⍣', '.', '∘', '⍤', '⍥', '@', '⌸', '⌺', '⍠', '&', '⌶', '⍛',
    ),

    zilde: _ => '⍬',

    number: _ => token(seq(
      optional('¯'),
      choice(/[0-9]+(\.[0-9]*)?/, /\.[0-9]+/, /∞/),
      optional(seq(/[eE]/, optional('¯'), /[0-9]+/)),
      optional(seq(/[jJ]/, optional('¯'),
        choice(/[0-9]+(\.[0-9]*)?/, /\.[0-9]+/),
        optional(seq(/[eE]/, optional('¯'), /[0-9]+/)))),
    )),

    string: _ => token(choice(
      /'([^'\n]|'')*'/,
      /"([^"\\\n]|\\.)*"/,
    )),

    identifier: _ => NAME,

    // ns.fn, #.a.b, ##.x, ⎕SE.x: names joined by dots, without spaces.
    qualified_name: _ => token(seq(
      choice(NAME, '##', '#', /⎕[A-Za-z]+/),
      repeat1(seq('.', NAME)),
    )),

    // ⎕IO, ⎕NA, ⎕ (quad output); also the roots # and ##.
    system_name: _ => token(choice(/⎕[A-Za-z]*/, '##', '#')),

    comment: _ => token(choice(/⍝[^\n]*/, /#![^\n]*/)),

    // Any other control word: :Leave :Continue :Return :GoTo :Access :Field
    // :Section :Require ... with the rest of its line.
    control_statement: $ => seq(
      field('keyword', $.keyword),
      optional($._expression),
    ),
    keyword: _ => token(/:[A-Za-z]+/),
  },
});
