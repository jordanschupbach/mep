/**
 * @file Tree-sitter grammar for Markdown's block structure.
 *
 * mep's own grammar, written against CommonMark 0.31 (spec.commonmark.org)
 * and GFM 0.29's extensions (pipe tables, task lists), plus YAML/TOML front
 * matter. Its node names, fields and tree shapes are the ones
 * tree-sitter-grammars/tree-sitter-markdown 0.5 established, which mep
 * vendored before (third_party/grammars/markdown), so queries written for
 * that grammar keep working; see README.md.
 *
 * Markdown is two grammars. This one reads the block structure (headings,
 * lists, block quotes, code, tables, ...) and leaves each run of inline text
 * as an `inline` node; ../tree-sitter-markdown_inline parses those ranges
 * (emphasis, links, code spans), which mep does per node with
 * ts_parser_set_included_ranges.
 *
 * Containers (block quotes, list items, indented code) live in the external
 * scanner (src/scanner.c): at each line's start it matches the open
 * containers -- a `>`, enough indentation -- emitting a `block_continuation`
 * per matched one and `_block_close` for the first that does not match,
 * and it decides whether a line break is soft (the paragraph goes on) by
 * looking at what the next line would start.
 *
 * Rules are declared in the old grammar's order, which is load-bearing:
 * where two parses survive to the same point, tree-sitter keeps the one
 * whose node types are numbered lower, and node types are numbered in
 * declaration order.
 */

const common = require('./common.js');

const PRECEDENCE_LEVEL_LINK = common.PRECEDENCE_LEVEL_LINK;

module.exports = grammar({
  name: 'markdown',

  rules: {
    // The text before the first heading is a section of its own; every ATX
    // heading opens one, holding the deeper ones after it.
    document: $ => seq(
      optional(choice($.minus_metadata, $.plus_metadata)),
      alias(prec.right(repeat($._block_not_section)), $.section),
      repeat($.section),
    ),

    ...common.rules,
    // Only the inline grammar marks a token as punctuation.
    _last_token_punctuation: _ => choice(),

    // ---- block structure ---------------------------------------------

    _block: $ => choice($._block_not_section, $.section),

    _block_not_section: $ => choice(
      alias($._setext_heading1, $.setext_heading),
      alias($._setext_heading2, $.setext_heading),
      $.paragraph,
      $.indented_code_block,
      $.block_quote,
      $.thematic_break,
      $.list,
      $.fenced_code_block,
      $._blank_line,
      $.html_block,
      $.link_reference_definition,
      $.pipe_table,
    ),

    section: $ => choice($._section1, $._section2, $._section3, $._section4, $._section5, $._section6),

    _section1: $ => prec.right(seq(
      alias($._atx_heading1, $.atx_heading),
      repeat(choice(
        alias(choice($._section6, $._section5, $._section4, $._section3, $._section2), $.section),
        $._block_not_section,
      )),
    )),
    _section2: $ => prec.right(seq(
      alias($._atx_heading2, $.atx_heading),
      repeat(choice(
        alias(choice($._section6, $._section5, $._section4, $._section3), $.section),
        $._block_not_section,
      )),
    )),
    _section3: $ => prec.right(seq(
      alias($._atx_heading3, $.atx_heading),
      repeat(choice(
        alias(choice($._section6, $._section5, $._section4), $.section),
        $._block_not_section,
      )),
    )),
    _section4: $ => prec.right(seq(
      alias($._atx_heading4, $.atx_heading),
      repeat(choice(
        alias(choice($._section6, $._section5), $.section),
        $._block_not_section,
      )),
    )),
    _section5: $ => prec.right(seq(
      alias($._atx_heading5, $.atx_heading),
      repeat(choice(
        alias($._section6, $.section),
        $._block_not_section,
      )),
    )),
    _section6: $ => prec.right(seq(
      alias($._atx_heading6, $.atx_heading),
      repeat($._block_not_section),
    )),

    // §4.1.
    thematic_break: $ => seq($._thematic_break, choice($._newline, $._eof)),

    // §4.2: `#` to `######`, then the heading's text.
    _atx_heading1: $ => prec(1, seq($.atx_h1_marker, optional($._atx_heading_content), $._newline)),
    _atx_heading2: $ => prec(1, seq($.atx_h2_marker, optional($._atx_heading_content), $._newline)),
    _atx_heading3: $ => prec(1, seq($.atx_h3_marker, optional($._atx_heading_content), $._newline)),
    _atx_heading4: $ => prec(1, seq($.atx_h4_marker, optional($._atx_heading_content), $._newline)),
    _atx_heading5: $ => prec(1, seq($.atx_h5_marker, optional($._atx_heading_content), $._newline)),
    _atx_heading6: $ => prec(1, seq($.atx_h6_marker, optional($._atx_heading_content), $._newline)),

    _atx_heading_content: $ => prec(1, seq(
      optional($._whitespace),
      field('heading_content', alias($._line, $.inline)),
    )),

    // §4.3: a paragraph and its `===` or `---` underline.
    _setext_heading1: $ => seq(
      field('heading_content', $.paragraph),
      $.setext_h1_underline,
      choice($._newline, $._eof),
    ),
    _setext_heading2: $ => seq(
      field('heading_content', $.paragraph),
      $.setext_h2_underline,
      choice($._newline, $._eof),
    ),

    // §4.4: chunks of lines indented four columns, blank lines between.
    indented_code_block: $ => prec.right(seq($._indented_chunk, repeat(choice($._indented_chunk, $._blank_line)))),
    _indented_chunk: $ => seq(
      $._indented_chunk_start,
      repeat(choice($._line, $._newline)),
      $._block_close,
      optional($.block_continuation),
    ),

    // §4.5: ``` or ~~~, an info string, the code, and a closing fence (or
    // the container's end).
    fenced_code_block: $ => prec.right(choice(
      seq(
        alias($._fenced_code_block_start_backtick, $.fenced_code_block_delimiter),
        optional($._whitespace),
        optional($.info_string),
        $._newline,
        optional($.code_fence_content),
        optional(seq(alias($._fenced_code_block_end_backtick, $.fenced_code_block_delimiter), $._close_block, $._newline)),
        $._block_close,
      ),
      seq(
        alias($._fenced_code_block_start_tilde, $.fenced_code_block_delimiter),
        optional($._whitespace),
        optional($.info_string),
        $._newline,
        optional($.code_fence_content),
        optional(seq(alias($._fenced_code_block_end_tilde, $.fenced_code_block_delimiter), $._close_block, $._newline)),
        $._block_close,
      ),
    )),

    code_fence_content: $ => repeat1(choice($._newline, $._line)),

    // The info string: a language, then anything; or Pandoc-style `{...}`.
    info_string: $ => choice(
      seq($.language, repeat(choice($._line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
      seq(
        repeat1(choice('{', '}')),
        optional(choice(
          seq($.language, repeat(choice($._line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
          seq($._whitespace, repeat(choice($._line, $.backslash_escape, $.entity_reference, $.numeric_character_reference))),
        )),
      ),
    ),

    language: $ => prec.right(repeat1(choice(
      $._word,
      common.punctuationWithout($, ['{', '}', ',']),
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
    ))),

    // §4.6: the seven kinds of HTML block, each told apart by how it
    // starts and ends (the scanner reads the starts).
    html_block: $ => prec(1, seq(optional($._whitespace), choice(
      $._html_block_1,
      $._html_block_2,
      $._html_block_3,
      $._html_block_4,
      $._html_block_5,
      $._html_block_6,
      $._html_block_7,
    ))),
    _html_block_1: $ => htmlBlock($, $._html_block_1_start, $._html_block_1_end),
    _html_block_2: $ => htmlBlock($, $._html_block_2_start, '-->'),
    _html_block_3: $ => htmlBlock($, $._html_block_3_start, '?>'),
    _html_block_4: $ => htmlBlock($, $._html_block_4_start, '>'),
    _html_block_5: $ => htmlBlock($, $._html_block_5_start, ']]>'),
    _html_block_6: $ => htmlBlock($, $._html_block_6_start, seq($._newline, $._blank_line)),
    _html_block_7: $ => htmlBlock($, $._html_block_7_start, seq($._newline, $._blank_line)),

    // §4.7: `[label]: destination "title"`.
    link_reference_definition: $ => prec.dynamic(PRECEDENCE_LEVEL_LINK, seq(
      optional($._whitespace),
      $.link_label,
      ':',
      optional(seq(optional($._whitespace), optional(seq($._soft_line_break, optional($._whitespace))))),
      $.link_destination,
      optional(prec.dynamic(2 * PRECEDENCE_LEVEL_LINK, seq(
        choice(
          seq($._whitespace, optional(seq($._soft_line_break, optional($._whitespace)))),
          seq($._soft_line_break, optional($._whitespace)),
        ),
        optional($._no_indented_chunk),
        $.link_title,
      ))),
      choice($._newline, $._soft_line_break, $._eof),
    )),

    _text_inline_no_link: $ => choice($._word, $._whitespace, common.punctuationWithout($, ['[', ']'])),

    // §4.8: lines of text up to a blank line or a block that interrupts.
    paragraph: $ => seq(
      alias(repeat1(choice($._line, $._soft_line_break)), $.inline),
      choice($._newline, $._eof),
    ),

    // §4.9.
    _blank_line: $ => seq($._blank_line_start, choice($._newline, $._eof)),

    // §5.1.
    block_quote: $ => seq(
      alias($._block_quote_start, $.block_quote_marker),
      optional($.block_continuation),
      repeat($._block),
      $._block_close,
      optional($.block_continuation),
    ),

    // §5.2, §5.3: a list is its items, all with the same kind of marker.
    list: $ => prec.right(choice($._list_plus, $._list_minus, $._list_star, $._list_dot, $._list_parenthesis)),
    _list_plus: $ => prec.right(repeat1(alias($._list_item_plus, $.list_item))),
    _list_minus: $ => prec.right(repeat1(alias($._list_item_minus, $.list_item))),
    _list_star: $ => prec.right(repeat1(alias($._list_item_star, $.list_item))),
    _list_dot: $ => prec.right(repeat1(alias($._list_item_dot, $.list_item))),
    _list_parenthesis: $ => prec.right(repeat1(alias($._list_item_parenthesis, $.list_item))),

    // A marker that may not interrupt a paragraph (an empty item, or an
    // ordered one not starting at 1) is a token of its own.
    list_marker_plus: $ => choice($._list_marker_plus, $._list_marker_plus_dont_interrupt),
    list_marker_minus: $ => choice($._list_marker_minus, $._list_marker_minus_dont_interrupt),
    list_marker_star: $ => choice($._list_marker_star, $._list_marker_star_dont_interrupt),
    list_marker_dot: $ => choice($._list_marker_dot, $._list_marker_dot_dont_interrupt),
    list_marker_parenthesis: $ => choice($._list_marker_parenthesis, $._list_marker_parenthesis_dont_interrupt),

    _list_item_plus: $ => listItem($, $.list_marker_plus),
    _list_item_minus: $ => listItem($, $.list_marker_minus),
    _list_item_star: $ => listItem($, $.list_marker_star),
    _list_item_dot: $ => listItem($, $.list_marker_dot),
    _list_item_parenthesis: $ => listItem($, $.list_marker_parenthesis),

    // An item starting with two blank lines is empty (§5.2); GFM's task
    // items start with `[ ]` or `[x]`.
    _list_item_content: $ => choice(
      prec(1, seq($._blank_line, $._blank_line, $._close_block, optional($.block_continuation))),
      repeat1($._block),
      prec(1, seq(
        choice($.task_list_marker_checked, $.task_list_marker_unchecked),
        $._whitespace,
        $.paragraph,
        repeat($._block),
      )),
    ),

    _newline: $ => seq($._line_ending, optional($.block_continuation)),

    _soft_line_break: $ => seq($._soft_line_ending, optional($.block_continuation)),

    _line: $ => prec.right(repeat1(choice($._word, $._whitespace, common.punctuationWithout($, [])))),

    _word: _ => choice(
      new RegExp('[^' + common.PUNCTUATION_CHARACTERS_REGEX + ' \\t\\n\\r]+'),
      /\[[xX]\]/,
      /\[[ \t]\]/,
    ),

    _whitespace: _ => /[ \t]+/,

    task_list_marker_checked: _ => prec(1, /\[[xX]\]/),
    task_list_marker_unchecked: _ => prec(1, /\[[ \t]\]/),

    // GFM §4.10: a header row, a delimiter row, and body rows.
    pipe_table: $ => prec.right(seq(
      $._pipe_table_start,
      alias($.pipe_table_row, $.pipe_table_header),
      $._newline,
      $.pipe_table_delimiter_row,
      repeat(seq($._pipe_table_newline, optional($.pipe_table_row))),
      choice($._newline, $._eof),
    )),

    _pipe_table_newline: $ => seq($._pipe_table_line_ending, optional($.block_continuation)),

    pipe_table_delimiter_row: $ => seq(
      optional(seq(optional($._whitespace), '|')),
      repeat1(prec.right(seq(
        optional($._whitespace),
        $.pipe_table_delimiter_cell,
        optional($._whitespace),
        '|',
      ))),
      optional($._whitespace),
      optional(seq($.pipe_table_delimiter_cell, optional($._whitespace))),
    ),

    pipe_table_delimiter_cell: $ => seq(
      optional(alias(':', $.pipe_table_align_left)),
      repeat1('-'),
      optional(alias(':', $.pipe_table_align_right)),
    ),

    pipe_table_row: $ => seq(
      optional(seq(optional($._whitespace), '|')),
      choice(
        seq(
          repeat1(prec.right(seq(
            choice(
              seq(optional($._whitespace), $.pipe_table_cell, optional($._whitespace)),
              alias($._whitespace, $.pipe_table_cell),
            ),
            '|',
          ))),
          optional($._whitespace),
          optional(seq($.pipe_table_cell, optional($._whitespace))),
        ),
        seq(optional($._whitespace), $.pipe_table_cell, optional($._whitespace)),
      ),
    ),

    pipe_table_cell: $ => prec.right(seq(
      choice($._word, $._backslash_escape, common.punctuationWithout($, ['|'])),
      repeat(choice($._word, $._whitespace, $._backslash_escape, common.punctuationWithout($, ['|']))),
    )),
  },

  externals: $ => [
    // A hard line ending: the paragraph (or other line) ends.
    $._line_ending,
    // A line ending inside a paragraph: the next line continues it.
    $._soft_line_ending,
    // Closes the innermost open container.
    $._block_close,
    // A matched container at a line's start: a `>`, or indentation.
    $.block_continuation,
    $._block_quote_start,
    $._indented_chunk_start,
    $.atx_h1_marker,
    $.atx_h2_marker,
    $.atx_h3_marker,
    $.atx_h4_marker,
    $.atx_h5_marker,
    $.atx_h6_marker,
    $.setext_h1_underline,
    $.setext_h2_underline,
    $._thematic_break,
    $._list_marker_minus,
    $._list_marker_plus,
    $._list_marker_star,
    $._list_marker_parenthesis,
    $._list_marker_dot,
    $._list_marker_minus_dont_interrupt,
    $._list_marker_plus_dont_interrupt,
    $._list_marker_star_dont_interrupt,
    $._list_marker_parenthesis_dont_interrupt,
    $._list_marker_dot_dont_interrupt,
    $._fenced_code_block_start_backtick,
    $._fenced_code_block_start_tilde,
    $._blank_line_start,
    $._fenced_code_block_end_backtick,
    $._fenced_code_block_end_tilde,
    $._html_block_1_start,
    $._html_block_1_end,
    $._html_block_2_start,
    $._html_block_3_start,
    $._html_block_4_start,
    $._html_block_5_start,
    $._html_block_6_start,
    $._html_block_7_start,
    // Asks the scanner to close the innermost block after the next line
    // ending (an HTML block's end condition, a fence's close).
    $._close_block,
    // Valid where an indented chunk may not start (a link title's line).
    $._no_indented_chunk,
    // Never produced on its own: ends a parse branch that read a line
    // break the wrong way.
    $._error,
    // Valid where a branch must end: the scanner answers with `_error`.
    $._trigger_error,
    $._eof,
    $.minus_metadata,
    $.plus_metadata,
    $._pipe_table_start,
    $._pipe_table_line_ending,
  ],

  precedences: $ => [
    [$._setext_heading1, $._block],
    [$._setext_heading2, $._block],
    [$.indented_code_block, $._block],
  ],

  conflicts: $ => [
    [$.link_reference_definition],
    [$.link_label, $._line],
    [$.link_reference_definition, $._line],
  ],

  extras: _ => [],
});

/**
 * An HTML block: its start, then lines until its end condition (which
 * closes the block at the next line ending) or the container's end.
 */
function htmlBlock($, open, close) {
  return seq(
    open,
    repeat(choice(
      $._line,
      $._newline,
      seq(close, $._close_block),
    )),
    $._block_close,
    optional($.block_continuation),
  );
}

/** A list item: its marker, then blocks, until the scanner closes it. */
function listItem($, marker) {
  return seq(
    marker,
    optional($.block_continuation),
    $._list_item_content,
    $._block_close,
    optional($.block_continuation),
  );
}
