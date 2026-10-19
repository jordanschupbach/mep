; Highlight captures for mep's Python grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (see TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after.

; Identifier naming conventions

(identifier) @variable

; CamelCase reads as a class; ALL_CAPS as a constant. The @constructor
; match demands a lowercase letter somewhere so an ALL_CAPS name matches
; only the @constant pattern below -- both patterns cover the identical
; span, and leaving that to the tiebreak alone is what used to colour the
; same MAX_SIZE Blue in one place and Cyan in another.
((identifier) @constructor
 (#match? @constructor "^[A-Z].*[a-z]"))

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z0-9_]*$"))

; The implicit first parameter of a method is a builtin, not a local.
((identifier) @variable.builtin
 (#any-of? @variable.builtin "self" "cls"))

; Parameters and arguments

; A name a signature *binds* -- and the `kw=` half of a keyword argument at
; a call site -- reads as a parameter rather than as an ordinary variable,
; so a def's own inputs stand out from the locals around them and a call's
; keyword names stand out from their values. Only the binding occurrence,
; not uses of the name in the body: telling a parameter's use apart from
; any other local needs scope resolution, which a query cannot do.
;
; self/cls are excluded so a method's implicit first parameter keeps the
; builtin colour the pattern above gives it -- both patterns cover the
; identical span, and the later one would otherwise win.
((parameters
   (identifier) @variable.parameter)
 (#not-any-of? @variable.parameter "self" "cls"))
(parameters
  (typed_parameter
    (identifier) @variable.parameter))
(parameters
  (default_parameter
    name: (identifier) @variable.parameter))
(parameters
  (typed_default_parameter
    name: (identifier) @variable.parameter))
; `*args` / `**kwargs`, bare or annotated -- the name, not the stars.
(parameters
  (list_splat_pattern
    (identifier) @variable.parameter))
(parameters
  (dictionary_splat_pattern
    (identifier) @variable.parameter))
(parameters
  (typed_parameter
    (list_splat_pattern
      (identifier) @variable.parameter)))
(parameters
  (typed_parameter
    (dictionary_splat_pattern
      (identifier) @variable.parameter)))

; A lambda's parameters are parameters too.
(lambda_parameters
  (identifier) @variable.parameter)
(lambda_parameters
  (default_parameter
    name: (identifier) @variable.parameter))
(lambda_parameters
  (list_splat_pattern
    (identifier) @variable.parameter))
(lambda_parameters
  (dictionary_splat_pattern
    (identifier) @variable.parameter))
(lambda_parameters
  (tuple_pattern
    (identifier) @variable.parameter))

; `f(timeout=3)` -- the keyword's name; the value after `=` is ordinary
; code and keeps the colours it would have anywhere else.
(keyword_argument
  name: (identifier) @variable.parameter)

; An attribute access reads as a member, not as a free variable. Ahead of
; the call patterns below so a method call's own name keeps the function
; colour even if @property is given a colour of its own -- the two cover
; the identical span, and the later pattern is the one that wins.
(attribute attribute: (identifier) @property)

; Function calls

; A decorator's own name (and its `@`), but not the arguments of a
; parametrised one -- (decorator) as a whole also spans `(maxsize=None)`,
; which is ordinary code and wants the ordinary code colours.
(decorator "@" @function)
(decorator
  [
    (identifier)
    (attribute)
  ] @function)
(decorator
  (call
    function: [
      (identifier)
      (attribute)
    ] @function))

(call
  function: (attribute attribute: (identifier) @function.method))
(call
  function: (identifier) @function)

; Builtin functions

((call
  function: (identifier) @function.builtin)
 (#match?
   @function.builtin
   "^(abs|aiter|anext|all|any|ascii|bin|bool|breakpoint|bytearray|bytes|callable|chr|classmethod|compile|complex|delattr|dict|dir|divmod|enumerate|eval|exec|filter|float|format|frozenset|getattr|globals|hasattr|hash|help|hex|id|input|int|isinstance|issubclass|iter|len|list|locals|map|max|memoryview|min|next|object|oct|open|ord|pow|print|property|range|repr|reversed|round|set|setattr|slice|sorted|staticmethod|str|sum|super|tuple|type|vars|zip|__import__)$"))

; Function definitions

(function_definition
  name: (identifier) @function)

; Type annotations -- the whole annotation, not just a bare name, so
; list[int] / Mapping[str, Widget] / a.b.C / int | None read as one type
; rather than as a type plus uncoloured punctuation and arguments. Each
; annotation shape is listed rather than using (type (_)), which would
; also swallow None/True/False and string annotations and repaint them.
(type
  [
    (identifier)
    (attribute)
    (subscript)
    (generic_type)
    (member_type)
    (union_type)
    (constrained_type)
    (splat_type)
    ; PEP 604's `int | None`, which the grammar parses as the ordinary
    ; binary operator it looks like rather than as a union_type.
    (binary_operator)
  ] @type)

; Literals

[
  (none)
  (true)
  (false)
  (ellipsis)
] @constant.builtin

[
  (integer)
  (float)
] @number

(comment) @comment

; A backslash joining two lines.
(line_continuation) @punctuation.special

; Strings are captured piece by piece rather than as one (string) span:
; an f-string's `{...}` fields are children of the string node too, and a
; span over the whole string would paint them (and everything they
; contain that has no narrower capture of its own -- names, attributes,
; operators) in the string colour, making a field indistinguishable from
; the literal text around it.
[
  (string_start)
  (string_content)
  (string_end)
] @string

(escape_sequence) @escape
; `{{` / `}}` -- a literal brace, part of the text rather than a field.
(escape_interpolation) @string.escape

; The field's own braces, its !r/!s/!a conversion and its :.2f format
; spec: each its own colour, with the expression between them left to the
; ordinary code captures above.
(interpolation
  "{" @punctuation.interpolation
  "}" @punctuation.interpolation)
(type_conversion) @character.special
(format_specifier) @string.special

[
  "-"
  "-="
  "!="
  "*"
  "**"
  "**="
  "*="
  "/"
  "//"
  "//="
  "/="
  "&"
  "&="
  "%"
  "%="
  "^"
  "^="
  "+"
  "->"
  "+="
  "<"
  "<<"
  "<<="
  "<="
  "<>"
  "="
  ":="
  "=="
  ">"
  ">="
  ">>"
  ">>="
  "|"
  "|="
  "~"
  "@="
] @operator

; The operators that are spelled as words are keywords too -- left in the
; @operator group above they rendered in the plain text colour, so `x in
; xs` or `a is not b` read as if they were bare identifiers.
[
  "and"
  "in"
  "is"
  "is not"
  "not"
  "not in"
  "or"
] @keyword.operator

[
  "as"
  "assert"
  "async"
  "await"
  "break"
  "class"
  "continue"
  "def"
  "del"
  "elif"
  "else"
  "except"
  "exec"
  "finally"
  "for"
  "from"
  "global"
  "if"
  "import"
  "lambda"
  "nonlocal"
  "pass"
  "print"
  "raise"
  "return"
  "try"
  "while"
  "with"
  "yield"
  "match"
  "case"
  "type"
  ; `_` only exists as a token in a match statement's wildcard pattern --
  ; a plain `_ = x` or `for _ in xs` is an identifier and stays one.
  "_"
] @keyword
