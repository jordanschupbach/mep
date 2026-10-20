/**
 * @file Tree-sitter grammar for Org.
 *
 * mep's own grammar, written against the Org syntax specification
 * (orgmode.org/worg/org-syntax.html) and Org 9.7's manual. Its node
 * names, fields and tree shapes are the ones the grammar mep vendored
 * before established (third_party/grammars/org: nvim-orgmode's
 * tree-sitter-org 2.0, extended with links, citations, inline source and
 * LaTeX math), so the queries written for it keep working; see README.md.
 *
 * Org's text is mostly words, and the tree keeps it that way: every
 * whitespace-separated word is an `expr` of `str` (letters), `num`
 * (digits), `sym` (any other single character) and punctuation tokens.
 * Elements are line-based: a line is an element's start by its first
 * characters (`* `, `#+`, `|`, `- `, ...), and the external scanner
 * (src/scanner.c) settles what needs memory: which headline ends a
 * section, where a list item ends, and `$` math.
 *
 * Rules are declared in the old grammar's order, which is load-bearing:
 * where two parses survive to the same point, tree-sitter keeps the one
 * whose node types are numbered lower, and node types are numbered in
 * declaration order.
 */

// Every ASCII punctuation character, a token of its own inside a word.
const PUNCTUATION = [
  '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
  ':', ';', '<', '=', '>', '?', '@', '[', '\\', ']', '^', '_', '`', '{', '|',
  '}', '~',
];

// Characters a word may start with that also start markup (a timestamp's
// `<` and `[`, a tag list's or drawer's `:`): there the word shares the
// markup's own token, and the parser, not the lexer, tells them apart.
const SHARED_START = ['<', '[', ':'];

// Lexical precedence must be a number to decide between tokens (the named
// ones in `precedences` order rules, not tokens): a table's `|`, a
// checkbox's `]` and inline code's `}` outrank a word's own.
const SPECIAL = 5;

const PIPE = token(prec(SPECIAL, '|'));

// A footnote's label: one token for definitions and references alike, so
// the line's end, not the lexer, tells `[fn:1] text` from `[fn:1]`.
const FN_LABEL = /[^\p{Z}\]:]+/;

const STR = /\p{L}+/;
const NUM = /\p{N}+/;
const SYM = /[^\p{Z}\p{L}\p{N}\t\n\r]/;

module.exports = grammar({
  name: 'org',

  // Newlines are explicit: elements are lines. Other blanks separate words.
  extras: _ => [/[\p{Z}\t\v\f﻿]/],

  externals: $ => [
    $._liststart,
    $._listend,
    $._listitemend,
    $.bullet,
    $._stars,
    $._sectionend,
    $._eof,
    $._link_open,
    $.latex_math_single_dollar,
    $.text_dollar,
    $.error_sentinel,
    $._drawer_start,
    $._property_drawer_start,
    $._block_begin,
    $._block_end,
    $._math_start,
    $._footnote_start,
  ],

  inline: $ => [
    $._nl,
    $._eol,
    $._ts_contents,
    $._directive_list,
    $._body_contents,
  ],

  precedences: _ => [
    ['special', 'immediate', 'non-immediate'],
  ],

  conflicts: $ => [
    // A line of timestamps after a headline is its plan; with anything
    // else on it, a paragraph. Only the line's end tells.
    [$.entry, $._markup],
    // Directives belong to the element that follows them, if one does;
    // otherwise (a blank line, a headline) they are the document's.
    [$._multis, $.paragraph, $.fndef, $.block, $.dynamic_block, $.list, $.table, $.latex_env],
    [$.item],
    [$.entry, $.expr],
    [$.expr],
    [$.fndef, $.fnref],
  ],

  rules: {
    document: $ => seq(
      optional(field('body', $.body)),
      repeat(field('subsection', $.section)),
    ),

    body: $ => $._body_contents,

    // [[url]] and [[url][description]]: the scanner has checked the line
    // holds a closing `]]` before it lets `[[` open one.
    link: $ => seq(
      alias($._link_open, '[['),
      field('url', alias($._link_target, $.expr)),
      ']]',
    ),

    link_desc: $ => seq(
      alias($._link_open, '[['),
      field('url', alias($._link_target, $.expr)),
      '][',
      field('desc', alias($._link_target, $.expr)),
      ']]',
    ),

    priority: _ => /\[#[A-Za-z0-9]\]/,

    // src_LANG{code} and src_LANG[args]{code}.
    inline_code_block: $ => seq(
      field('open', alias($._inline_code_open, $.open)),
      optional(field('contents', alias($._code_contents, $.contents))),
      field('close', alias(token(prec(SPECIAL, '}')), $.close)),
    ),

    // One token through the `{`, so a word that merely contains `src_`
    // (`mep_org_src_block_at`) is not inline code.
    _inline_code_open: _ => choice(
      /src_[^\p{Z}\[\]{}\n\r]+\{/,
      /src_[^\p{Z}\[\]{}\n\r]+\[[^\]\n\r]*\]\{/,
    ),

    // \(...\), and $...$ (the scanner decides when a `$` opens math). Each
    // opens only when its closer follows on the line (`_math_start`), so
    // a lone `\(` -- shown in =verbatim= text, say -- is text.
    inline_math_block: $ => choice(
      seq(
        $._math_start,
        field('open', alias('\\(', $.open)),
        field('contents', alias($._math_contents, $.contents)),
        field('close', alias('\\)', $.close)),
      ),
      seq(
        field('open', alias($.latex_math_single_dollar, $.open)),
        field('contents', alias($._math_contents, $.contents)),
        field('close', alias($.latex_math_single_dollar, $.close)),
      ),
    ),

    // \[...\] and $$...$$.
    display_math_block: $ => choice(
      seq(
        $._math_start,
        field('open', alias('\\[', $.open)),
        field('contents', alias($._math_contents, $.contents)),
        field('close', alias('\\]', $.close)),
      ),
      seq(
        $._math_start,
        field('open', alias('$$', $.open)),
        field('contents', alias($._math_contents, $.contents)),
        field('close', alias('$$', $.close)),
      ),
    ),

    // An element, or a directive of the document's own (`#+TITLE:`) that
    // no element follows.
    _multis: $ => choice(
      $._element,
      field('directive', $.directive),
    ),

    _element: $ => choice(
      $.comment,
      $.fndef,
      $.drawer,
      $.list,
      $.block,
      $.dynamic_block,
      $.table,
      $.paragraph,
      $.latex_env,
    ),

    // ---- headlines ---------------------------------------------------

    section: $ => seq(
      field('headline', $.headline),
      optional(field('plan', $.plan)),
      optional(field('property_drawer', $.property_drawer)),
      optional(field('body', $.body)),
      repeat(field('subsection', $.section)),
      $._sectionend,
    ),

    stars: $ => seq(prec.dynamic(10, $._stars), /\*+/),

    headline: $ => seq(
      field('stars', $.stars),
      /[ \t]+/,
      optional(field('item', $.item)),
      optional(field('tags', $.tag_list)),
      $._eol,
    ),

    item: $ => repeat1(choice($.expr, $._markup, field('priority', $.priority))),

    tag_list: $ => prec.dynamic(1, seq(
      ':',
      repeat1(seq(field('tag', alias($._tag_expr_start, $.tag)), immediate(':'))),
    )),

    // A tag's characters are a word's own tokens, so `* :emoji: Title`
    // stays a title until the line's end shows it is not tags.
    _tag_expr_start: $ => repeat1(choice(
      alias(immediate(STR), 'str'),
      alias(immediate(NUM), 'num'),
      ...['_', '@', '#', '%'].map(c => immediate(c)),
    )),

    property_drawer: $ => seq(
      $._property_drawer_start,
      caseInsensitive(':properties:'),
      repeat1($._nl),
      repeat(seq($.property, repeat1($._nl))),
      prec.dynamic(1, caseInsensitive(':end:')),
      $._eol,
    ),

    property: $ => seq(
      ':',
      field('name', alias($._noc_expr, $.expr)),
      immediate(':'),
      optional(field('value', alias($._expr_line, $.value))),
    ),

    plan: $ => prec.dynamic(1, seq(repeat1($.entry), $._eol)),

    entry: $ => seq(
      optional(seq(field('name', alias(nonImmediate(STR), $.entry_name)), immediate(':'))),
      field('timestamp', $.timestamp),
    ),

    // <active>, [inactive], ranges, and <%%(sexp)>.
    timestamp: $ => choice(
      seq('<', $._ts_contents, choice('>', seq('>--<', $._ts_contents, '>'))),
      seq('[', $._ts_contents, choice(']', seq(']--[', $._ts_contents, ']'))),
      seq('<%%', $.tsexp, token.immediate('>')),
      seq('[%%', $.tsexp, token.immediate(']')),
    ),

    tsexp: $ => repeat1(alias($._ts_expr, $.expr)),

    _ts_contents: $ => seq(field('date', $.date), repeat($._ts_element)),

    date: _ => /\p{N}{1,4}-\p{N}{1,2}-\p{N}{1,4}/,

    _ts_element: $ => choice(
      field('day', alias(/\p{L}[^\]>\p{Z}\p{N}\n\r]*/, $.day)),
      field('time', alias(/\p{N}?\p{N}:\p{N}\p{N}([AaPp][Mm])?/, $.time)),
      field('duration', alias(/\p{N}?\p{N}:\p{N}\p{N}([AaPp][Mm])?-\p{N}?\p{N}:\p{N}\p{N}([AaPp][Mm])?/, $.duration)),
      field('repeat', alias(/[.+]?\+\p{N}+\p{L}/, $.repeat)),
      field('delay', alias(/--?\p{N}+\p{L}/, $.delay)),
      alias(prec(-1, /[^\]>\p{Z}\n\r]+/), $.expr),
    ),

    // ---- elements ----------------------------------------------------

    paragraph: $ => seq(
      optional($._directive_list),
      $._multiline_text,
    ),

    // [fn:LABEL] DESCRIPTION, running on until a blank line.
    // At a line's start a definition, if text follows on the line; a lone
    // `[fn:1]` is a paragraph's reference.
    fndef: $ => prec.dynamic(1, seq(
      optional($._directive_list),
      $._footnote_start,
      caseInsensitive('[fn:'),
      field('label', alias(FN_LABEL, $.expr)),
      ']',
      field('description', alias($._fndef_description, $.description)),
    )),

    // A directive right before an element is the element's (`#+NAME:`,
    // `#+CAPTION:`), not the document's.
    _directive_list: $ => repeat1(prec.dynamic(1, field('directive', $.directive))),

    directive: $ => seq(
      '#+',
      field('name', alias($._noc_expr, $.expr)),
      immediate(':'),
      optional(field('value', alias($._expr_line, $.value))),
      $._eol,
    ),

    comment: $ => prec.right(repeat1(seq(
      token(prec(SPECIAL, /#[^+\n\r]/)),
      repeat($.expr),
      $._eol,
    ))),

    // :NAME: on a line of its own (the scanner checks the line), then
    // lines of text up to :END:.
    drawer: $ => seq(
      $._drawer_start,
      ':',
      field('name', alias($._noc_expr, $.expr)),
      immediate(':'),
      $._nl,
      optional(field('contents', alias($._drawer_contents, $.contents))),
      caseInsensitive(':end:'),
      $._eol,
    ),

    // #+begin_NAME ... #+end_NAME: only an end with the block's own name
    // closes it (the scanner remembers the name), so a block may hold a
    // differently named one's lines (`#+begin_details` around
    // `#+begin_summary`, `#+begin_example` around a source block) as text.
    block: $ => seq(
      optional($._directive_list),
      $._block_begin,
      caseInsensitive('#+begin_'),
      field('name', alias($._immediate_expr, $.expr)),
      repeat(field('parameter', $.expr)),
      $._nl,
      optional(field('contents', alias($._block_contents, $.contents))),
      $._block_end,
      caseInsensitive('#+end_'),
      field('end_name', alias($._immediate_expr, $.expr)),
      $._eol,
    ),

    dynamic_block: $ => seq(
      optional($._directive_list),
      caseInsensitive('#+begin:'),
      field('name', $.expr),
      repeat(field('parameter', $.expr)),
      $._nl,
      optional(field('contents', alias($._block_contents, $.contents))),
      caseInsensitive('#+end:'),
      $._eol,
    ),

    // Lines inside a block are text, even when they look like elements;
    // each ends in its newline, so `#+end_` closes the block only at a
    // line's start (`,#+end_src` is an escaped line of the contents).
    _block_contents: $ => repeat1(seq(repeat($.expr), $._nl)),

    // A drawer's lines are text with markup (a LOGBOOK's clocked times).
    _drawer_contents: $ => repeat1(seq(repeat(choice($.expr, $._markup)), $._nl)),

    list: $ => seq(
      optional($._directive_list),
      $._liststart,
      repeat(seq($.listitem, $._listitemend, repeat($._nl))),
      seq($.listitem, $._listend),
    ),

    listitem: $ => seq(
      field('bullet', $.bullet),
      optional(field('checkbox', $.checkbox)),
      choice($._eof, field('contents', $._body_contents)),
    ),

    // The closing `]` outranks a word's in the lexer, so `- [X] done` is a
    // checkbox.
    checkbox: $ => choice(
      '[ ]',
      seq('[', field('status', alias($._checkbox_status_expr, $.expr)), token(prec(SPECIAL, ']'))),
    ),

    table: $ => prec.right(seq(
      optional($._directive_list),
      repeat1(choice($.row, $.hr)),
      repeat($.formula),
    )),

    // A cell's `|`, a row's closing `|` and a rule line's first are one
    // token: at a line's start, above a word's `|`.
    row: $ => prec(1, seq(
      repeat1($.cell),
      optional(PIPE),
      $._eol,
    )),

    cell: $ => prec.right(seq(
      PIPE,
      optional(field('contents', $.contents)),
    )),

    hr: $ => seq(
      PIPE,
      repeat1(seq(token.immediate(/[-+]+/), optional(token.immediate('|')))),
      $._eol,
    ),

    formula: $ => seq(
      caseInsensitive('#+tblfm:'),
      repeat1(field('formula', $.expr)),
      $._eol,
    ),

    // \begin{NAME}...\end{NAME}, and \[ on a line of its own ... \].
    latex_env: $ => seq(
      optional($._directive_list),
      choice(
        seq(
          caseInsensitive('\\begin{'),
          field('name', alias(/[\p{L}\p{N}*]+/, $.name)),
          token.immediate('}'),
          $._nl,
          optional(field('contents', alias($._block_contents, $.contents))),
          caseInsensitive('\\end{'),
          alias(/[\p{L}\p{N}*]+/, $.name),
          token.immediate('}'),
        ),
        seq(
          /\\\[[ \t]*(\r?\n|\r)/,
          optional(field('contents', alias($._block_contents, $.contents))),
          '\\]',
        ),
      ),
      $._eol,
    ),

    contents: $ => prec.right(repeat1(choice($.expr, $._markup))),

    // Math holds no markup (and does not nest); inline code ends at its
    // first `}`.
    _math_contents: $ => repeat1($.expr),

    _code_contents: $ => repeat1(alias($._code_expr, $.expr)),

    _code_expr: _ => seq(
      expr('non-immediate', nonImmediate, '}'),
      repeat(expr('immediate', immediate, '}')),
    ),

    // ---- text --------------------------------------------------------

    _nl: _ => choice('\n', '\r'),

    _eol: $ => choice('\n', '\r', $._eof),

    _body_contents: $ => prec.right(repeat1(choice($._nl, $._multis))),

    _expr_line: $ => repeat1(choice($.expr, $._markup)),

    // A footnote's description runs on to the next blank line. Each
    // continuation line's first word is not an `expr` but its bare tokens
    // (the old grammar's shape, kept).
    _fndef_description: $ => prec.right(seq(
      repeat1(choice($.expr, $._markup)),
      // A line starting with `[fn:` is the next definition.
      repeat(seq(
        $._nl,
        choice(
          $._fndef_initial_expr,
          $.link,
          $.link_desc,
          $.citation,
          $.timestamp,
          $.inline_code_block,
          $.inline_math_block,
          $.display_math_block,
        ),
        repeat(choice($.expr, $._markup)),
      )),
      $._eol,
    )),

    _fndef_initial_expr: _ => seq(
      expr('non-immediate', nonImmediate),
      repeat(expr('immediate', immediate)),
    ),

    // A paragraph's lines run on to a blank line or another element: two
    // paragraphs are never adjacent. (An element's first token tells: the
    // scanner marks a drawer's `:NAME:` line, whose `:` a word shares.)
    _multiline_text: $ => prec.right(repeat1(seq(repeat1(choice($.expr, $._markup)), $._eol))),

    _markup: $ => choice(
      $.link,
      $.link_desc,
      $.fnref,
      $.citation,
      $.timestamp,
      $.inline_code_block,
      $.inline_math_block,
      $.display_math_block,
    ),

    // [fn:LABEL], and the inline definitions [fn::TEXT] and
    // [fn:LABEL:TEXT] (which the old grammar could not parse), the text a
    // `description` running to the closing `]`, across lines.
    fnref: $ => seq(
      $._footnote_start,
      caseInsensitive('[fn:'),
      choice(
        seq(field('label', alias(FN_LABEL, $.expr)), ']'),
        seq(
          optional(field('label', alias(FN_LABEL, $.expr))),
          immediate(':'),
          field('description', alias($._inline_footnote, $.description)),
          ']',
        ),
      ),
    ),

    _inline_footnote: $ => repeat1(choice(alias($._footnote_expr, $.expr), $._markup, $._nl)),

    _footnote_expr: _ => seq(
      expr('non-immediate', nonImmediate, ']'),
      repeat(expr('immediate', immediate, ']')),
    ),

    // [cite:@key] and [cite/style:prefix @key suffix; @other].
    citation: $ => seq(
      choice(
        '[cite:',
        seq('[cite/', field('style', alias(/[^:\]\p{Z}\n\r]+/, $.expr)), ':'),
      ),
      repeat(choice(
        field('reference', $.citation_reference),
        alias($._expr_with_space, $.expr),
      )),
      ']',
    ),

    citation_reference: $ => seq('@', field('key', alias(/[^\p{Z}\];\n\r]+/, $.expr))),

    _expr_with_space: _ => /[^@\]\n\r]+/,

    // A link's target or description: words and the blanks between them
    // (kept as tokens), a lone `]` included; `]]` and `][` end it.
    _link_target: _ => repeat1(choice(
      expr('immediate', immediate),
      token.immediate(' '),
    )),

    _immediate_expr: $ => repeat1(expr('immediate', immediate)),

    _noc_expr: $ => repeat1(expr('immediate', immediate, ':')),

    _checkbox_status_expr: $ => expr('immediate', immediate, ']'),

    _ts_expr: $ => seq(
      expr('non-immediate', nonImmediate, '>]'),
      repeat(expr('immediate', immediate, '>]')),
    ),

    expr: $ => seq(
      expr('non-immediate', nonImmediate),
      repeat(expr('immediate', immediate)),
    ),
  },
});

/** A word's first token: below every element's own tokens. */
function nonImmediate(rule) {
  return SHARED_START.includes(rule) ? rule : token(prec('non-immediate', rule));
}

/** A token continuing a word, with no blank before it. */
function immediate(rule) {
  return token.immediate(prec('immediate', rule));
}

/**
 * One character class of a word: a punctuation character, a run of
 * letters (`str`) or digits (`num`), or any other single character
 * (`sym`), each made a token by `tokenize` (nonImmediate or immediate).
 */
function expr(precedence, tokenize, skip = '') {
  return prec(precedence, choice(
    ...PUNCTUATION.filter(c => !skip.includes(c)).map(c => tokenize(c)),
    alias(tokenize(STR), 'str'),
    alias(tokenize(NUM), 'num'),
    alias(tokenize(SYM), 'sym'),
  ));
}

/** An ASCII keyword matched in any case, keeping its lower-case spelling as the node name. */
function caseInsensitive(keyword) {
  return alias(
    new RegExp(keyword.replace(/[\\^$.*+?()[\]{}|]/g, '\\$&').replace(/[a-zA-Z]/g, l => `[${l.toLowerCase()}${l.toUpperCase()}]`)),
    keyword,
  );
}
