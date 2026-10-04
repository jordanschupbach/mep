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
    $._directive_start, // zero-width: the line is a known \directive (or @directive)
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
    $._paren_open,
    $._paren_close,
    $._arg_comma, // `, ` ending the argument of \name(arg, text)
    ...EMPHASIS.flatMap(([, open, close]) => [$[open], $[close]]),
    $._abstract_open,
    $._abstract_break, // a line break (and any blank lines) inside \abstract()
    $._result_begin_markdown, // `// result_begin: markdown`
    $._result_end_attached, // `// result_end` with a \caption/\alttext under it
    $._attribute_start, // zero-width: the line is a \caption or \alttext
    $._slide_start, // zero-width: the line opens a slide
    $._slide_end, // zero-width: the line closes the open slide
    $._command_block_start, // zero-width: \define(, or a \raw( / user command's call on lines of its own
    $._opaque_text, // a \define's template or a \raw's text: up to its closing `)`, unparsed
    $._cmd_raw, // `\raw` before (formats, text)
    $._cmd_user, // `\name` of a user command (or \when, \otherwise) before (args)
    $.math_trailing, // text after a display-maths closer, up to the end of its line
    $._box_start, // zero-width: the line opens a box (\definition( ...) whose content follows
    $._box_line_start, // zero-width: the line opens a box that closes where its text ends
    $._box_open, // a box's `(`
    $._box_break, // a line break inside a box's first paragraph
    $._box_end, // zero-width: the line closes the open box
    $._cmd_class, // `\class` before (name, text)
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
      $._markdown_results_end,
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
      $.abstract,
      $.slide,
      $.box,
      $.define,
      $.raw_block,
      $.command_block,
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
      optional(choice($.results, $._markdown_results_begin)),
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

    // results=markdown: the Markdown the block printed, between the
    // markers, is the document's own blocks (a table it printed is a table).
    // The block ends at the opening marker; the closing one stands alone,
    // or is the captioned table's (image's, maths') just above its caption.
    _markdown_results_begin: $ => seq(alias($._result_begin_markdown, $.result_begin), $._newline),
    _markdown_results_end: $ => seq($.result_end, $._newline),
    _results_attributes: $ => prec.right(seq(alias($._result_end_attached, $.result_end), $._newline, repeat1($._attribute))),

    // --- $$ ... $$ and \[ ... \] ------------------------------------------
    // Text after the closer (`$$.`, `\] and so on`) still closes the block,
    // as mepml_doc.cpp's ParseDisplayMath does (it warns instead). Without
    // math_trailing the rule failed at that line, and error recovery
    // re-ran math_content -- a scan to the next `$$` -- again and again:
    // quadratic in the rest of the document, 1.7 s a keystroke on 4000
    // lines (plans/MEPML_PERFORMANCE_PLAN.md).
    display_math: $ => prec.right(seq(
      $.math_open,
      optional($.math_content),
      $.math_close,
      optional($.math_trailing),
      $._line_end,
      choice(repeat($._attribute), $._results_attributes),
    )),

    // --- tables -------------------------------------------------------------
    table: $ => prec.right(seq(
      repeat1(choice($.table_row, seq($.table_delimiter_row, $._newline))),
      choice(repeat($._attribute), $._results_attributes),
    )),
    // Outer pipes are optional (GitHub-flavoured Markdown).
    table_row: $ => seq(
      $._table_row_start,
      choice(
        seq($._table_pipe, repeat(seq(optional($.table_cell), $._table_pipe)), optional($.table_cell)),
        seq($.table_cell, repeat1(seq($._table_pipe, optional($.table_cell)))),
      ),
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

    // --- \directives -----------------------------------------------------
    // Each is `\name(...)` (or bare, `\toc`); the older `@name{...}`
    // spelling still parses, except for the abstract. BibTeX entries keep
    // their `@type{...}`.
    _line_end: $ => seq(optional($._ws), optional($.inline_comment), $._newline),

    import: $ => seq($._directive_start, choice(seq('\\', 'import', $._paren_path_group), seq('@', 'import', $._path_group)), $._line_end),
    image: $ => prec.right(seq(
      $._directive_start, choice(seq('\\', 'image', $._paren_path_group), seq('@', 'image', $._path_group)), $._line_end,
      choice(repeat($._attribute), $._results_attributes),
    )),
    _path_group: $ => seq('{', optional(field('path', $.path)), '}'),
    path: _ => /[^}\n]+/,
    _paren_path_group: $ => seq('(', optional(field('path', alias($._paren_path, $.path))), ')'),
    _paren_path: _ => /[^)\n]+/,

    _attribute: $ => choice($.caption, $.alttext),
    caption: $ => seq(
      $._attribute_start,
      choice(seq('\\', 'caption', $._paren_content_group), seq('@', 'caption', $._content_group)),
      $._line_end,
    ),
    alttext: $ => seq(
      $._attribute_start,
      choice(
        seq('\\', 'alttext', '(', optional(alias($._paren_text, $.description)), ')'),
        seq('@', 'alttext', '{', optional(alias($._brace_text, $.description)), '}'),
      ),
      $._line_end,
    ),

    // @printbibliography is the directive's oldest name, still accepted.
    bibliography: $ => seq($._directive_start, choice(seq('\\', 'bibliography'), seq('@', choice('bibliography', 'printbibliography'))), $._line_end),
    toc: $ => seq($._directive_start, choice('\\', '@'), 'toc', $._line_end),

    // \abstract( prose over any number of lines; blank lines split paragraphs )
    abstract: $ => seq(
      $._directive_start, '\\', 'abstract',
      alias($._abstract_open, '('),
      optional(alias(repeat1(choice($._inline, $._abstract_break)), $.content)),
      alias($._paren_close, ')'),
      $._line_end,
    ),

    // \define(name(p1, p2), template): a user command. The template is
    // text -- parsed only where a call expands it.
    define: $ => seq(
      $._command_block_start, '\\', 'define', '(',
      optional($._ws), field('name', $.command_name),
      optional(seq(
        alias(/[ \t]*\(/, '('),
        optional(seq(field('parameter', $.parameter), repeat(seq($._comma, field('parameter', $.parameter))))),
        alias(/[ \t]*\)/, ')'),
      )),
      optional(seq(alias(/[ \t]*,/, ','), optional(field('template', alias($._opaque_text, $.template))))),
      ')',
      $._line_end,
    ),
    command_name: _ => /[A-Za-z]+/,
    parameter: _ => /[A-Za-z_][A-Za-z0-9_]*/,

    // \raw(formats, text) on lines of its own: the exports named get the
    // text as it is.
    raw_block: $ => seq(
      $._command_block_start, '\\', 'raw', '(',
      optional($._ws), field('formats', $.formats), alias(/[ \t]*,/, ','),
      optional(field('text', alias($._opaque_text, $.raw_text))),
      ')',
      $._line_end,
    ),
    formats: _ => /[^,()\s][^,()\n]*/,

    // A user command's call on lines of its own (its arguments prose over
    // any number of lines, as an abstract's are); \when(...) and
    // \otherwise(...) too.
    command_block: $ => seq(
      $._command_block_start, '\\', field('name', $.command_name),
      alias($._abstract_open, '('),
      optional(alias(repeat1(choice($._inline, $._abstract_break)), $.content)),
      alias($._paren_close, ')'),
      $._line_end,
    ),

    // \slide( on a line of its own, the slide's content -- any blocks --
    // and a line holding just its `)`; or `\slide{` / `@slide{` ... `}`.
    // Slides do not nest (the scanner opens no slide inside one).
    slide: $ => seq(
      $.slide_open,
      repeat(choice($._block, $._blank_line, $._any_section)),
      $.slide_close,
    ),
    slide_open: $ => seq(
      $._slide_start, optional($._ws),
      choice(seq('\\', 'slide', choice('(', '{')), seq('@', 'slide', '{')),
      $._line_end,
    ),
    slide_close: $ => seq($._slide_end, optional($._ws), choice(')', '}'), $._line_end),

    // A titled box: `\definition(Title,` on a line of its own, its content
    // -- any blocks -- and a line holding just `)`; or one closed where its
    // text ends, `\remark(Title, text)`. Text after the title (the comma
    // is part of the content) is the box's first paragraph. Boxes nest.
    box: $ => choice(
      seq(
        $.box_open,
        repeat(choice($._block, $._blank_line, $._any_section)),
        $.box_close,
      ),
      alias($._box_line, $.box_open),
    ),
    box_open: $ => seq(
      $._box_start, optional($._ws), '\\', field('kind', $.box_kind),
      alias($._box_open, '('),
      optional(alias(repeat1(choice($._inline, $._box_break)), $.content)),
      $._newline,
    ),
    _box_line: $ => seq(
      $._box_line_start, optional($._ws), '\\', field('kind', $.box_kind),
      alias($._box_open, '('),
      optional(alias(repeat1(choice($._inline, $._box_break)), $.content)),
      alias($._paren_close, ')'),
      $._line_end,
    ),
    // (`boxed` is any kind of the document's own: `\\boxed(axiom, Title,`.)
    box_kind: _ => choice('definition', 'theorem', 'lemma', 'proposition', 'corollary', 'fact', 'example', 'remark', 'proof', 'note', 'tip', 'warning', 'boxed'),
    box_close: $ => seq($._box_end, optional($._ws), ')', $._line_end),

    // \citation(key, field = value, ...) or @citation{key}{fields}
    citation: $ => seq(
      $._directive_start,
      choice(
        seq(
          '\\', 'citation',
          '(', optional($._space), field('key', alias($._paren_key, $.citation_key)), optional($._space), ',',
          repeat(choice(alias($._paren_field, $.citation_field), ',', $._space)), ')',
        ),
        seq(
          '@', 'citation',
          '{', optional($._space), field('key', $.citation_key), optional($._space), '}',
          optional($._space),
          '{', repeat(choice($.citation_field, ',', $._space)), '}',
        ),
      ),
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
    // Inside \citation(...): a field's bare value stops at its `)`.
    _paren_key: _ => /[^\s{}(),]+/,
    _paren_field: $ => seq(
      field('name', $.field_name),
      optional($._space), '=', optional($._space),
      field('value', choice($.braced_value, $.string, alias($._paren_bare_value, $.bare_value))),
    ),
    _paren_bare_value: _ => /[^,{}()"\s][^,}()\n]*/,
    _space: _ => /[ \t\r\n]+/,
    // Balanced-brace text (nested groups allowed, \{ escapes).
    _brace_text: $ => repeat1(choice(/[^{}\\]+/, /\\./, seq('{', optional($._brace_text), '}'))),
    // Balanced-parenthesis text (nested pairs allowed, \) escapes).
    _paren_text: $ => repeat1(choice(/[^()\\]+/, /\\./, seq('(', optional($._paren_text), ')'))),

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
      $.class,
      $.footnote,
      $.cite,
      $.citep,
      $.raw,
      $.command,
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
      alias($._alttext_marker, '\\alttext'),
      choice($._paren_arg_group, $._arg_group),
    ),

    link: $ => seq(
      alias($._link_open, '['),
      choice(
        seq(alias(repeat1($._inline), $.link_text), alias($._link_bar, '|'), $.url),
        $.url,
      ),
      alias($._link_close, ']'),
    ),

    // \name(arg, text) -- or the older \name{arg}{text}.
    font: $ => seq(alias($._cmd_f, '\\f'), choice($._paren_arg_content, seq($._arg_group, $._content_group))),
    font_size: $ => seq(alias($._cmd_fs, '\\fs'), choice($._paren_arg_content, seq($._arg_group, $._content_group))),
    color: $ => seq(alias($._cmd_color, '\\color'), choice($._paren_arg_content, seq($._arg_group, $._content_group))),
    // \class(name, text): text a style sheet selects by name (`.name`).
    class: $ => seq(alias($._cmd_class, '\\class'), choice($._paren_arg_content, seq($._arg_group, $._content_group))),
    footnote: $ => seq(alias($._cmd_fn, '\\fn'), choice($._paren_content_group, $._content_group)),
    cite: $ => seq(alias($._cmd_cite, '\\cite'), choice($._paren_arg_group, $._arg_group)),
    citep: $ => seq(alias($._cmd_citep, '\\citep'), choice($._paren_arg_group, $._arg_group)),

    // \raw(formats, text) in prose, and a user command's call.
    raw: $ => seq(
      alias($._cmd_raw, '\\raw'),
      alias($._paren_open, '('),
      optional(alias($._arg_text, $.formats)),
      alias($._arg_comma, ','),
      optional(alias($._opaque_text, $.raw_text)),
      alias($._paren_close, ')'),
    ),
    command: $ => seq(alias($._cmd_user, $.command_name), $._paren_content_group),

    _paren_arg_group: $ => seq(
      alias($._paren_open, '('),
      optional(alias($._arg_text, $.argument)),
      alias($._paren_close, ')'),
    ),
    _paren_content_group: $ => seq(
      alias($._paren_open, '('),
      optional(alias(repeat1(choice($._inline, $._soft_break)), $.content)),
      alias($._paren_close, ')'),
    ),
    _paren_arg_content: $ => seq(
      alias($._paren_open, '('),
      optional(alias($._arg_text, $.argument)),
      alias($._arg_comma, ','),
      optional(alias(repeat1(choice($._inline, $._soft_break)), $.content)),
      alias($._paren_close, ')'),
    ),

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
