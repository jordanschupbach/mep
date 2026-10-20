/**
 * @file The rules mep's two Markdown grammars share: the block grammar
 * (./grammar.js) and the inline one (../tree-sitter-markdown_inline), which
 * mep parses over each block's `inline` ranges. Link reference definitions
 * are block structure but their labels, destinations and titles are inline
 * syntax, so both need them; escapes and references appear in both.
 *
 * Written against CommonMark 0.31 (spec.commonmark.org) and GFM 0.29.
 */

// CommonMark §2.1: ASCII punctuation.
const PUNCTUATION_CHARACTERS_REGEX = '!-/:-@\\[-`\\{-~';
const PUNCTUATION_CHARACTERS_ARRAY = [
  '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
  ':', ';', '<', '=', '>', '?', '@', '[', '\\', ']', '^', '_', '`', '{', '|',
  '}', '~',
];

// Links and images outrank the readings they compete with (GLR dynamic
// precedence), as the spec's "links take precedence" rules say.
const PRECEDENCE_LEVEL_LINK = 10;

// §2.5: every entity name HTML5 defines (WHATWG's list), `&name;`.
const HTML_ENTITIES = require('./html_entities.json');

module.exports.PUNCTUATION_CHARACTERS_REGEX = PUNCTUATION_CHARACTERS_REGEX;
module.exports.PUNCTUATION_CHARACTERS_ARRAY = PUNCTUATION_CHARACTERS_ARRAY;
module.exports.PRECEDENCE_LEVEL_LINK = PRECEDENCE_LEVEL_LINK;

/**
 * Every punctuation character but `chars`, each its own token, each
 * optionally followed by the inline grammar's `_last_token_punctuation`
 * marker (the block grammar defines that as nothing).
 */
function punctuationWithout($, chars) {
  return seq(
    choice(...PUNCTUATION_CHARACTERS_ARRAY.filter(c => !chars.includes(c))),
    optional($._last_token_punctuation),
  );
}
module.exports.punctuationWithout = punctuationWithout;

module.exports.rules = {
  // §2.4: a backslash before ASCII punctuation.
  backslash_escape: $ => $._backslash_escape,
  _backslash_escape: _ => new RegExp('\\\\[' + PUNCTUATION_CHARACTERS_REGEX + ']'),

  // §2.5: `&amp;` and `&#123;`/`&#x1F;`.
  entity_reference: _ => new RegExp('&(' + HTML_ENTITIES.join('|') + ');'),
  numeric_character_reference: _ => /&#([0-9]{1,7}|[xX][0-9a-fA-F]{1,6});/,

  // §4.7: `[label]`, at least one character, no unescaped brackets.
  link_label: $ => seq('[', repeat1(choice(
    $._text_inline_no_link,
    $.backslash_escape,
    $.entity_reference,
    $.numeric_character_reference,
    $._soft_line_break,
  )), ']'),

  // §6.3: `<anything but < > or a line end>`, or a run with balanced
  // parentheses and no blanks.
  link_destination: $ => prec.dynamic(PRECEDENCE_LEVEL_LINK, choice(
    seq('<', repeat(choice(
      $._text_no_angle,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
    )), '>'),
    seq(
      choice(
        $._word,
        punctuationWithout($, ['<', '(', ')']),
        $.backslash_escape,
        $.entity_reference,
        $.numeric_character_reference,
        $._link_destination_parenthesis,
      ),
      repeat(choice(
        $._word,
        punctuationWithout($, ['(', ')']),
        $.backslash_escape,
        $.entity_reference,
        $.numeric_character_reference,
        $._link_destination_parenthesis,
      )),
    ),
  )),

  _link_destination_parenthesis: $ => seq('(', repeat(choice(
    $._word,
    punctuationWithout($, ['(', ')']),
    $.backslash_escape,
    $.entity_reference,
    $.numeric_character_reference,
    $._link_destination_parenthesis,
  )), ')'),

  _text_no_angle: $ => choice($._word, punctuationWithout($, ['<', '>']), $._whitespace),

  // §6.3: "...", '...' or (...), which may span lines but not a blank one.
  link_title: $ => choice(
    seq('"', repeat(choice(
      $._word,
      punctuationWithout($, ['"']),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), '"'),
    seq('\'', repeat(choice(
      $._word,
      punctuationWithout($, ['\'']),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), '\''),
    seq('(', repeat(choice(
      $._word,
      punctuationWithout($, ['(', ')']),
      $._whitespace,
      $.backslash_escape,
      $.entity_reference,
      $.numeric_character_reference,
      seq($._soft_line_break, optional(seq($._soft_line_break, $._trigger_error))),
    )), ')'),
  ),
};
