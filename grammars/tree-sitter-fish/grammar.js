"use strict";
// mep's fish grammar. Started from ram02z/tree-sitter-fish 3.7.0's
// grammar.js (released into the public domain under the Unlicense), whose
// node types and fields it keeps, so queries written for that grammar -- the
// one mep loaded from nixpkgs before -- carry over. What changed, each
// checked against fish's own tokenizer and parser (see README.md): `(` in
// double quotes is text; `{` where a command starts is a block; `&` and `^`
// inside words; blanks in brace expansions; newlines after `&&`, `||` and
// `|`; line continuations and the comments they carry over; a last
// statement with no newline after it; `for x in` with no values.
// TODO(9):     Go through SPECIAL_CARACTERS for `word` and `bracket_word` and ensure they are correct.
function regexChars(characters) {
    const regexSpecialCharacters = [
        '$', '\\', '*', '(', ')', '{', '}', '[', ']', '|', '^'
    ];
    return characters
        .map((c) => regexSpecialCharacters.includes(c) ? '\\' + c : c)
        .join('');
}
// https://util.unicode.org/UnicodeJsps/list-unicodeset.jsp?a=%5B%3AWhite_Space%3DYes%3A%5D&g=&i=
const WHITESPACE = /[\u0009-\u000D\u0085\u2028\u2029\u0020\u3000\u1680\u2000-\u2006\u2008-\u200A\u205F\u00A0\u2007\u202F]+/;
// Set of characters that cannot start a word.
const WORD_START_NEG_PATTERN = regexChars([
    '$',
    '*',
    '~',
    '#',
    '(', ')',
    '{', '}',
    '[', ']',
    '<', '>',
    '"', "'",
    '&',
    '|',
    ';',
    '\\',
    '\\s',
]);
// Set of characters that cannot continue a word.
const WORD_CONTINUE_NEG_PATTERN = regexChars([
    '$',
    '*',
    '~',
    '(', ')',
    '{', '}',
    '[', ']',
    '<', '>',
    '"', "'",
    '&',
    '|',
    ';',
    '\\',
    '\\s',
]);
// `&` is part of a word when a word character follows it (fish 3.4's
// ampersand-nobg-in-token, on by default): `a&b` is one argument, `a &` and
// `a&` run in the background.
const AMP_IN_WORD = `&[^${WORD_CONTINUE_NEG_PATTERN}#]`;
const WORD_PATTERN = new RegExp(`(?:[^${WORD_START_NEG_PATTERN}]|${AMP_IN_WORD})(?:[^${WORD_CONTINUE_NEG_PATTERN}]|${AMP_IN_WORD})*`);
module.exports = grammar({
    name: 'fish',
    externals: $ => [
        $._concat,
        $._brace_concat,
        $._concat_list,
        $._begin_brace,
        // `\` + newline. An argument where one may stand (an escape_sequence,
        // as the upstream grammar has always read it), invisible elsewhere
        // (`end \` then `| cmd` on the next line).
        $._line_continuation,
        // A comment the scanner reads itself: only one on a line that a
        // continuation carried over, whose newline continues the line too.
        $.comment,
        // That comment's newline.
        $._comment_newline,
        // Never produced: valid only inside quotes, so the scanner can tell.
        $._in_string,
        // A continuation where a command would start (`a | \\` then `b` on
        // the next line): never the command's name, always invisible.
        $._line_continuation_gap,
    ],
    inline: $ => [
        $._terminator,
        $._statement,
        $._base_expression,
    ],
    extras: $ => [
        $.comment,
        WHITESPACE,
        $._line_continuation,
        $._comment_newline,
        $._line_continuation_gap,
    ],
    word: ($) => $.word,
    rules: {
        // The last statement needs no terminator (a file without a final newline).
        program: $ => seq(repeat(seq(optional($._statement), $._terminator)), optional($._statement)),
        // fish skips newlines (and comments) after `&&`, `||` and a pipe.
        conditional_execution: $ => choice(prec.right(-1, seq(choice('and', 'or'), $._statement)), prec.right(-1, seq($._statement, choice('||', '&&'), repeat($._newline), $._statement))),
        pipe: $ => prec.left(seq($._statement, choice('&|', '2>|', '|'), repeat($._newline), $._statement)),
        _newline: () => choice('\n', '\r', '\r\n'),
        redirect_statement: $ => seq($._statement, choice($.file_redirect, $.stream_redirect)),
        _terminator: () => choice(';', '&', '\n', '\r', '\r\n'),
        _statement: $ => choice($.conditional_execution, $.pipe, $.command, $.redirect_statement, $.begin_statement, $.if_statement, $.while_statement, $.for_statement, $.switch_statement, $.function_definition, $.break, $.continue, $.return, $.negated_statement),
        _terminated_statement: $ => seq($._statement, $._terminator),
        _terminated_opt_statement: $ => seq(optional($._statement), $._terminator),
        negated_statement: $ => prec.left(-1, seq(choice('!', 'not'), $._statement)),
        /* Syntax `$(cmd)` added in 3.4.0. */
        command_substitution: $ => seq(optional('$'), '(', repeat(seq(optional($._statement), $._terminator)), optional($._statement), ')'),
        function_definition: $ => seq('function', field('name', $._expression), repeat(field('option', $._expression)), $._terminator, repeat($._terminated_statement), 'end'),
        integer: () => /(-|\+)?\d+/,
        float: () => /(-|\+)?\d+\.\d+/,
        return: $ => prec.left(seq('return', optional($._expression))),
        switch_statement: $ => seq('switch', field('value', $._expression), $._terminator, optional(repeat1($.case_clause)), 'end'),
        case_clause: $ => seq('case', repeat1($._expression), $._terminator, optional(repeat1($._terminated_statement))),
        break: () => 'break',
        continue: () => 'continue',
        // `for x in; end` is valid: the list may be empty. The variable is any
        // word to fish's parser (`for a,b in` fails only when it runs).
        for_statement: $ => seq('for', field('variable', choice($.variable_name, alias($.word, $.variable_name))), 'in', repeat(field('value', $._expression)), $._terminator, optional(repeat1($._terminated_statement)), 'end'),
        while_statement: $ => seq('while', field('condition', $._terminated_statement), optional(repeat1($._terminated_opt_statement)), 'end'),
        if_statement: $ => seq('if', field('condition', $._terminated_statement), optional(repeat1($._terminated_opt_statement)), repeat($.else_if_clause), optional($.else_clause), 'end'),
        else_if_clause: $ => seq(seq('else', 'if'), field('condition', $._terminated_statement), optional(repeat1($._terminated_opt_statement))),
        else_clause: $ => seq('else', $._terminator, optional(repeat1($._terminated_opt_statement))),
        /* Syntax `{ [COMMANDS ...] }` added in 4.1.0 */
        begin_statement: $ => choice(seq('begin', optional(repeat1($._terminated_opt_statement)), 'end'), seq(alias($._begin_brace, '{'), repeat($._terminated_opt_statement), optional($._statement), '}')),
        comment: () => token(prec(-11, /#.*/)),
        variable_name: () => /[a-zA-Z0-9_]+/,
        variable_expansion: $ => prec.left(seq('$', choice($.variable_name, $.variable_expansion), repeat(seq($._concat_list, $.list_element_access)))),
        index: $ => choice($.integer, $.single_quote_string, $.variable_expansion, $.double_quote_string, $.command_substitution),
        range: $ => prec.right(2, seq(optional($.index), '..', optional($.index))),
        list_element_access: $ => seq('[', repeat(choice($.index, $.range)), ']'),
        brace_expansion: $ => prec.right(seq('{', seq(optional($._brace_expression), repeat(seq(',', optional($._brace_expression)))), '}')),
        double_quote_string: $ => seq('"', repeat(choice(/[^\$\\"]+/, $.variable_expansion, $.escape_sequence, $._in_string, alias($._line_continuation, $.escape_sequence), 
        /*
         * Only new "$()" syntax is expanded inside double quoted strings.
         * However, "()" matches the regex above - so we're good.
         */
        alias($._quoted_command_substitution, $.command_substitution))), '"'),
        // Inside double quotes only `$(...)` substitutes; a bare `(` is text.
        _quoted_command_substitution: $ => seq('$', token.immediate('('), repeat(seq(optional($._statement), $._terminator)), optional($._statement), ')'),
        single_quote_string: $ => seq('\'', repeat(choice(/[^'\\]+/, $.escape_sequence, $._in_string, alias($._line_continuation, $.escape_sequence))), '\''),
        escape_sequence: () => token(seq('\\', token.immediate(choice(/[^xXuUc]/, /[0-7]{1,3}/, /x[0-9a-fA-F]{0,2}/, /X[0-9a-fA-F]{0,2}/, /u[0-9a-fA-F]{0,4}/, /U[0-9a-fA-F]{0,8}/, /c[a-zA-Z]?/)))),
        command: $ => prec.right(seq(field('name', $._expression), repeat(choice(field('redirect', choice($.file_redirect, $.stream_redirect)), field('argument', $._expression))))),
        stream_redirect: () => /\d*(>>|>|<)&[012-]/,
        direction: () => /(\d*|&)(>>?\??|<)/,
        file_redirect: $ => seq(field('operator', $.direction), field('destination', $._expression)),
        _special_character: () => choice('[', ']'),
        concatenation: $ => seq(choice($._base_expression, $._special_character), repeat1(seq($._concat, choice($._base_expression, $._special_character, '#')))),
        _expression: $ => choice($._base_expression, $.concatenation, alias($._special_character, $.word)),
        _base_expression: $ => choice($.command_substitution, $.single_quote_string, $.double_quote_string, $.variable_expansion, $.word, $.integer, $.float, $.brace_expansion, $.escape_sequence, alias($._line_continuation, $.escape_sequence), $.glob, $.home_dir_expansion),
        brace_concatenation: $ => seq(choice($._base_brace_expression, $.brace_expansion), repeat1(seq($._brace_concat, choice($._base_brace_expression, $.brace_expansion)))),
        _brace_expression: $ => choice(alias($.brace_concatenation, $.concatenation), $._base_brace_expression, $.brace_expansion),
        _base_brace_expression: $ => choice($.command_substitution, $.single_quote_string, $.double_quote_string, $.variable_expansion, alias($.brace_word, $.word), $.integer, $.float, $.escape_sequence, alias($._line_continuation, $.escape_sequence), $.glob),
        home_dir_expansion: () => '~',
        glob: () => token(repeat1('*')),
        word: () => WORD_PATTERN,
        // A brace element's leading blanks are not part of it (fish trims them).
        brace_word: () => new RegExp(`[^\\s${regexChars(['$', '\'', '*', '"', ',', '\\', '{', '}', '(', ')'])}][^${regexChars(['$', '\'', '*', '"', ',', '\\', '{', '}', '(', ')'])}]*`),
    },
});
