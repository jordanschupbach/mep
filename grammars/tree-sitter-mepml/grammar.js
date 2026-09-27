/**
 * @file Tree-sitter grammar for mepml, mep's literate-programming markup.
 *
 * The language itself is defined by mep's own parser (src/mepml_doc.cpp)
 * and specified by example in test.mepml; this grammar produces the same
 * block structure and inline markup for tree-sitter consumers (mep's
 * highlighter, folds and outline; any other editor).
 *
 * mepml is line-oriented and its inline markers are context-sensitive (a
 * `*` only opens bold after a space or punctuation, and only if a matching
 * closer follows in the same paragraph), so nearly every token comes from
 * the hand-written external scanner in src/scanner.c; this file only
 * arranges them. There are no `extras`: whitespace is significant.
 */

const CALLOUTS = [
  'NOTE', 'WARNING', 'ERROR', 'INFO', 'TIP', 'HINT', 'IMPORTANT', 'CAUTION',
  'TODO', 'FIXME', 'DANGER', 'SUCCESS', 'QUESTION', 'EXAMPLE',
];

const BIBTEX_TYPES = [
  'article', 'book', 'booklet', 'conference', 'inbook', 'incollection',
  'inproceedings', 'manual', 'mastersthesis', 'misc', 'phdthesis',
  'proceedings', 'techreport', 'unpublished', 'online', 'software',
];

// [name, open token, close token] for every paired inline marker.
const EMPHASIS = [
  ['bold', '_open_bold', '_close_bold'],
  ['italic', '_open_italic', '_close_italic'],
  ['underline', '_open_underline', '_close_underline'],
  ['superscript', '_open_sup', '_close_sup'],
  ['subscript', '_open_sub', '_close_sub'],
  ['small', '_open_small', '_close_small'],
  ['big', '_open_big', '_close_big'],
  ['monospace', '_open_mono', '_close_mono'],
  ['highlight', '_open_highlight', '_close_highlight'],
  ['strikethrough', '_open_strike', '_close_strike'],
  ['insertion', '_open_insert', '_close_insert'],
  ['deletion', '_open_delete', '_close_delete'],
];

module.exports = grammar({
  name: 'mepml',

  extras: _ => [],

  externals: $ => [
    // Block level (all decided at the start of a line).
    $._blank_line,
    $._newline,
    $.h1_marker, $.h2_marker, $.h3_marker, $.h4_marker, $.h5_marker, $.h6_marker,
    $.meta_marker,
    $.comment_marker,
    $.callout_marker,
    $.result_begin,
    $.result_end,
    $.fence_open,
    $.fence_close,
    $.code_content,
    $.math_open,
    $.math_content,
    $.math_close,
    $._directive_start, // zero-width: the line is a known @directive
    $._table_row_start, // zero-width: the line is a table row
    $.table_delimiter_row,
    $._table_pipe,
    $.list_marker,
    $.task_marker,
    $._list_continuation, // zero-width: an indented continuation line
    $.rule,
    $._paragraph_start, // zero-width: the line is paragraph text
    // Inline.
    $._text,
    $.escape,
    $.verbatim,
    $.latex,
    $._alttext_marker,
    $.inline_comment,
    $._link_open,
    $._link_bar,
    $.url,
    $._link_close,
    $._cmd_f, $._cmd_fs, $._cmd_color, $._cmd_fn, $._cmd_cite, $._cmd_citep,
    $._group_open,
    $._group_close,
    $._arg_text,
    ...EMPHASIS.flatMap(([, open, close]) => [$[open], $[close]]),
    $._error_sentinel,
  ],

  rules: {
    // Sections nest by heading depth: a level-N section holds everything up
    // to the next heading of level N or shallower, which is exactly what
    // it cannot contain -- so the parse table closes it with no scanner
    // state (tree-sitter-markdown's trick).
    document: $ => repeat(choice($._block, $._blank_line, $._any_section)),
    _any_section: $ => alias(choice($._section1, $._section2, $._section3, $._section4, $._section5, $._section6), $.section),
    ...Object.fromEntries([1, 2, 3, 4, 5, 6].map(n => [
      `_section${n}`,
      $ => prec.right(seq(
        alias($[`_heading${n}`], $.heading),
        repeat(choice(
          $._block,
          $._blank_line,
          ...(n < 6 ? [alias(choice(...[2, 3, 4, 5, 6].filter(k => k > n).map(k => $[`_section${k}`])), $.section)] : []),
        )),
      )),
    ])),
    ...Object.fromEntries([1, 2, 3, 4, 5, 6].map(n => [
      `_heading${n}`,
      $ => seq($[`h${n}_marker`], repeat($._inline), $._newline),
    ])),

    _block: $ => choice(
      $.meta,
      $.comment,
      $.callout,
      $.code_block,
      $.display_math,
      $.table,
      $.list,
      $.rule_line,
      $.import,
      $.image,
      $.citation,
      $.bibtex_entry,
      $.bibliography,
      $.toc,
      $.caption,
      $.alttext,
      $.paragraph,
    ),

    _ws: _ => /[ \t]+/,

    // --- //? Key: value ------------------------------------------------
    meta: $ => seq(
      $.meta_marker,
      optional($._ws),
      optional(field('key', $.meta_key)),
      optional(seq(':', optional($._ws), optional(field('value', $.meta_value)))),
      $._newline,
    ),
    meta_key: _ => /[^:\s][^:\n]*/,
    meta_value: _ => /[^ \t\n][^\n]*/,

    // --- // comments and callouts ---------------------------------------
    comment: $ => prec.right(seq(
      $.comment_marker,
      optional($.comment_text),
      repeat(seq($._newline, $.comment_marker, optional($.comment_text))),
      $._newline,
    )),
    comment_text: _ => /[^\n]+/,

    callout: $ => prec.right(seq(
      $.callout_marker,
      field('kind', $.callout_keyword),
      ':',
      repeat($._inline),
      repeat(seq($._newline, $.comment_marker, repeat($._inline))),
      $._newline,
    )),
    callout_keyword: _ => choice(...CALLOUTS),

    // --- ```{lang, opts} code blocks and their results ------------------
    code_block: $ => prec.right(seq(
      $.fence_open,
      optional($.info),
      optional($._ws),
      $._newline,
      optional($.code_content),
      $.fence_close,
      $._newline,
      optional($.results),
      repeat($._attribute),
    )),
    // Spacing inside the header is folded into its punctuation tokens,
    // so the lexer, not the parser, decides where a list continues.
    info: $ => choice(
      seq(alias(/\{[ \t]*/, '{'), field('language', $.language), repeat(seq($._comma, $.option)), alias(/[ \t]*\}/, '}')),
      seq(
        field('language', $.language),
        optional(seq(
          alias(/[ \t]*\{[ \t]*/, '{'),
          optional(seq($.option, repeat(seq($._comma, $.option)))),
          alias(/[ \t]*\}/, '}'),
        )),
      ),
    ),
    _comma: _ => alias(/[ \t]*,[ \t]*/, ','),
    language: _ => /[A-Za-z0-9_+#.\-]+/,
    option: $ => seq(
      field('name', $.option_name),
      optional(seq(alias(/[ \t]*=[ \t]*/, '='), field('value', $.option_value))),
    ),
    option_name: _ => /[A-Za-z_][A-Za-z0-9_.\-]*/,
    option_value: $ => choice($.string, /[^,}\s"]+/),
    string: _ => /"([^"\\\n]|\\.)*"/,

    results: $ => seq(
      $.result_begin,
      $._newline,
      repeat(seq($.comment_marker, optional($.result_text), $._newline)),
      $.result_end,
      $._newline,
    ),
    result_text: _ => /[^\n]+/,

    // --- $$ ... $$ and \[ ... \] ------------------------------------------
    display_math: $ => prec.right(seq(
      $.math_open,
      optional($.math_content),
      $.math_close,
      $._line_end,
      repeat($._attribute),
    )),

    // --- tables -------------------------------------------------------------
    table: $ => prec.right(seq(
      repeat1(choice($.table_row, seq($.table_delimiter_row, $._newline))),
      repeat($._attribute),
    )),
    table_row: $ => seq(
      $._table_row_start,
      $._table_pipe,
      repeat(seq(optional($.table_cell), $._table_pipe)),
      $._newline,
    ),
    table_cell: $ => repeat1($._inline),

    // --- lists --------------------------------------------------------------
    list: $ => prec.right(repeat1($.list_item)),
    list_item: $ => prec.right(seq(
      $.list_marker,
      optional($.task_marker),
      repeat($._inline),
      repeat(seq($._newline, $._list_continuation, repeat1($._inline))),
      $._newline,
    )),

    rule_line: $ => seq($.rule, $._newline),

    // --- @directives ------------------------------------------------------
    _line_end: $ => seq(optional($._ws), optional($.inline_comment), $._newline),

    import: $ => seq($._directive_start, '@', 'import', $._path_group, $._line_end),
    image: $ => prec.right(seq(
      $._directive_start, '@', 'image', $._path_group, $._line_end,
      repeat($._attribute),
    )),
    _path_group: $ => seq('{', optional(field('path', $.path)), '}'),
    path: _ => /[^}\n]+/,

    _attribute: $ => choice($.caption, $.alttext),
    caption: $ => seq($._directive_start, '@', 'caption', $._content_group, $._line_end),
    alttext: $ => seq($._directive_start, '@', 'alttext', '{', optional(alias($._brace_text, $.description)), '}', $._line_end),

    // @printbibliography is the directive's older name, still accepted.
    bibliography: $ => seq($._directive_start, '@', choice('bibliography', 'printbibliography'), $._line_end),
    toc: $ => seq($._directive_start, '@', 'toc', $._line_end),

    citation: $ => seq(
      $._directive_start, '@', 'citation',
      '{', optional($._space), field('key', $.citation_key), optional($._space), '}',
      optional($._space),
      '{', repeat(choice($.citation_field, ',', $._space)), '}',
      $._line_end,
    ),
    bibtex_entry: $ => seq(
      $._directive_start, '@', field('type', $.entry_type),
      '{', optional($._space), field('key', $.citation_key),
      repeat(choice($.citation_field, ',', $._space)),
      '}',
      $._line_end,
    ),
    entry_type: _ => choice(...BIBTEX_TYPES),
    citation_key: _ => /[^\s{},]+/,
    citation_field: $ => seq(
      field('name', $.field_name),
      optional($._space), '=', optional($._space),
      field('value', choice($.braced_value, $.string, $.bare_value)),
    ),
    field_name: _ => /[A-Za-z_][A-Za-z0-9_\-]*/,
    braced_value: $ => seq('{', optional($._brace_text), '}'),
    bare_value: _ => /[^,{}"\s][^,}\n]*/,
    _space: _ => /[ \t\r\n]+/,
    // Balanced-brace text (nested groups allowed, \{ escapes).
    _brace_text: $ => repeat1(choice(/[^{}\\]+/, /\\./, seq('{', optional($._brace_text), '}'))),

    // --- paragraphs ------------------------------------------------------------
    paragraph: $ => prec.right(seq(
      $._paragraph_start,
      repeat1(choice($._inline, $._soft_break)),
      $._newline,
    )),
    _soft_break: $ => prec(1, seq($._newline, $._paragraph_start)),

    // --- inline ------------------------------------------------------------------
    _inline: $ => choice(
      $._text,
      $.escape,
      $.verbatim,
      $.inline_math,
      $.inline_comment,
      $.link,
      ...EMPHASIS.map(([name]) => $[name]),
      $.font,
      $.font_size,
      $.color,
      $.footnote,
      $.cite,
      $.citep,
    ),

    ...Object.fromEntries(EMPHASIS.map(([name, open, close]) => [
      name,
      $ => seq(
        alias($[open], $.delimiter),
        repeat1(choice($._inline, $._soft_break)),
        alias($[close], $.delimiter),
      ),
    ])),

    inline_math: $ => seq($.latex, optional($.alttext_attribute)),
    alttext_attribute: $ => seq(
      alias($._alttext_marker, '@alttext'),
      $._arg_group,
    ),

    link: $ => seq(
      alias($._link_open, '['),
      choice(
        seq(alias(repeat1($._inline), $.link_text), alias($._link_bar, '|'), $.url),
        $.url,
      ),
      alias($._link_close, ']'),
    ),

    font: $ => seq(alias($._cmd_f, '\\f'), $._arg_group, $._content_group),
    font_size: $ => seq(alias($._cmd_fs, '\\fs'), $._arg_group, $._content_group),
    color: $ => seq(alias($._cmd_color, '\\color'), $._arg_group, $._content_group),
    footnote: $ => seq(alias($._cmd_fn, '\\fn'), $._content_group),
    cite: $ => seq(alias($._cmd_cite, '\\cite'), $._arg_group),
    citep: $ => seq(alias($._cmd_citep, '\\citep'), $._arg_group),

    _arg_group: $ => seq(
      alias($._group_open, '{'),
      optional(alias($._arg_text, $.argument)),
      alias($._group_close, '}'),
    ),
    _content_group: $ => seq(
      alias($._group_open, '{'),
      optional(alias(repeat1(choice($._inline, $._soft_break)), $.content)),
      alias($._group_close, '}'),
    ),
  },
});
