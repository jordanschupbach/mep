#pragma once

// Embedded verbatim from each grammar repo's queries/highlights.scm at
// the tag pinned in CMakeLists.txt (add_ts_grammar / the TS_GRAMMARS
// table), fetched at vendoring time -- not regenerated at build time, so
// mep's build has no extra network dependency. A handful of grammars
// ship no highlights.scm of their own (hcl, kotlin, verilog, graphql,
// just); those five come from nvim-treesitter's community query set
// instead (github.com/nvim-treesitter/nvim-treesitter, MIT licensed),
// matched against the exact grammar/node-type version vendored here.
// Predicates ((#match? ...), (#eq? ...), (#any-of? ...), and
// nvim-treesitter's own (#lua-match? ...) alias) are evaluated at
// runtime by EvalPredicates in treesitter.cpp; scope-only predicates
// ((#is-not? local) and friends) are accepted as-is -- see the comment
// there for the exact list this integration does and doesn't handle.

static const char *kHighlightsC = R"TSQ(
(identifier) @variable

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z\\d_]*$"))

"break" @keyword
"case" @keyword
"const" @keyword
"continue" @keyword
"default" @keyword
"do" @keyword
"else" @keyword
"enum" @keyword
"extern" @keyword
"for" @keyword
"if" @keyword
"inline" @keyword
"return" @keyword
"sizeof" @keyword
"static" @keyword
"struct" @keyword
"switch" @keyword
"typedef" @keyword
"union" @keyword
"volatile" @keyword
"while" @keyword

"#define" @keyword
"#elif" @keyword
"#else" @keyword
"#endif" @keyword
"#if" @keyword
"#ifdef" @keyword
"#ifndef" @keyword
"#include" @keyword
(preproc_directive) @keyword

"--" @operator
"-" @operator
"-=" @operator
"->" @operator
"=" @operator
"!=" @operator
"*" @operator
"&" @operator
"&&" @operator
"+" @operator
"++" @operator
"+=" @operator
"<" @operator
"==" @operator
">" @operator
"||" @operator

"." @delimiter
";" @delimiter

(string_literal) @string
(system_lib_string) @string

(null) @constant
(number_literal) @number
(char_literal) @number

(field_identifier) @property
(statement_identifier) @label
(type_identifier) @type
(primitive_type) @type
(sized_type_specifier) @type

(call_expression
  function: (identifier) @function)
(call_expression
  function: (field_expression
    field: (field_identifier) @function))
(function_declarator
  declarator: (identifier) @function)
(preproc_function_def
  name: (identifier) @function.special)

(comment) @comment
)TSQ";

static const char *kHighlightsLua = R"TSQ(
; Keywords
"return" @keyword.return

[
  "goto"
  "in"
  "local"
  "global"
] @keyword

(label_statement) @label

(break_statement) @keyword

(do_statement
  [
    "do"
    "end"
  ] @keyword)

(while_statement
  [
    "while"
    "do"
    "end"
  ] @repeat)

(repeat_statement
  [
    "repeat"
    "until"
  ] @repeat)

(if_statement
  [
    "if"
    "elseif"
    "else"
    "then"
    "end"
  ] @conditional)

(elseif_statement
  [
    "elseif"
    "then"
    "end"
  ] @conditional)

(else_statement
  [
    "else"
    "end"
  ] @conditional)

(for_statement
  [
    "for"
    "do"
    "end"
  ] @repeat)

(function_declaration
  [
    "function"
    "end"
  ] @keyword.function)

(function_definition
  [
    "function"
    "end"
  ] @keyword.function)

; Operators
(binary_expression
  operator: _ @operator)

(unary_expression
  operator: _ @operator)

"=" @operator

[
  "and"
  "not"
  "or"
] @keyword.operator

; Punctuations
[
  ";"
  ":"
  ","
  "."
] @punctuation.delimiter

; Brackets
[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

; Variables
(identifier) @variable

((identifier) @variable.builtin
  (#eq? @variable.builtin "self"))

(variable_list
  (attribute
    "<" @punctuation.bracket
    (identifier) @attribute
    ">" @punctuation.bracket))

; Constants
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z_0-9]*$"))

(vararg_expression) @constant

(nil) @constant.builtin

[
  (false)
  (true)
] @boolean

; Tables
(field
  name: (identifier) @field)

(dot_index_expression
  field: (identifier) @field)

(table_constructor
  [
    "{"
    "}"
  ] @constructor)

; Functions
(parameters
  (identifier) @parameter)

(function_declaration
  name: [
    (identifier) @function
    (dot_index_expression
      field: (identifier) @function)
  ])

(function_declaration
  name: (method_index_expression
    method: (identifier) @method))

(assignment_statement
  (variable_list
    .
    name: [
      (identifier) @function
      (dot_index_expression
        field: (identifier) @function)
    ])
  (expression_list
    .
    value: (function_definition)))

(table_constructor
  (field
    name: (identifier) @function
    value: (function_definition)))

(function_call
  name: [
    (identifier) @function.call
    (dot_index_expression
      field: (identifier) @function.call)
    (method_index_expression
      method: (identifier) @method.call)
  ])

(function_call
  (identifier) @function.builtin
  (#any-of? @function.builtin
    ; built-in functions in Lua 5.1
    "assert" "collectgarbage" "dofile" "error" "getfenv" "getmetatable" "ipairs" "load" "loadfile"
    "loadstring" "module" "next" "pairs" "pcall" "print" "rawequal" "rawget" "rawset" "require"
    "select" "setfenv" "setmetatable" "tonumber" "tostring" "type" "unpack" "xpcall"))

; Others
(comment) @comment

(hash_bang_line) @preproc

(number) @number

(string) @string

(escape_sequence) @string.escape
)TSQ";

static const char *kHighlightsPython = R"TSQ(
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
] @constant.builtin

[
  (integer)
  (float)
] @number

(comment) @comment

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
)TSQ";

static const char *kHighlightsJavascript = R"TSQ(
; Variables
;----------

(identifier) @variable

; Properties
;-----------

(property_identifier) @property

; Function and method definitions
;--------------------------------

(function_expression
  name: (identifier) @function)
(function_declaration
  name: (identifier) @function)
(method_definition
  name: (property_identifier) @function.method)

(pair
  key: (property_identifier) @function.method
  value: [(function_expression) (arrow_function)])

(assignment_expression
  left: (member_expression
    property: (property_identifier) @function.method)
  right: [(function_expression) (arrow_function)])

(variable_declarator
  name: (identifier) @function
  value: [(function_expression) (arrow_function)])

(assignment_expression
  left: (identifier) @function
  right: [(function_expression) (arrow_function)])

; Function and method calls
;--------------------------

(call_expression
  function: (identifier) @function)

(call_expression
  function: (member_expression
    property: (property_identifier) @function.method))

; Special identifiers
;--------------------

((identifier) @constructor
 (#match? @constructor "^[A-Z]"))

([
    (identifier)
    (shorthand_property_identifier)
    (shorthand_property_identifier_pattern)
 ] @constant
 (#match? @constant "^[A-Z_][A-Z\\d_]+$"))

((identifier) @variable.builtin
 (#match? @variable.builtin "^(arguments|module|console|window|document)$")
 (#is-not? local))

((identifier) @function.builtin
 (#eq? @function.builtin "require")
 (#is-not? local))

; Literals
;---------

(this) @variable.builtin
(super) @variable.builtin

[
  (true)
  (false)
  (null)
  (undefined)
] @constant.builtin

(comment) @comment

[
  (string)
  (template_string)
] @string

(regex) @string.special
(number) @number

; Tokens
;-------

[
  ";"
  (optional_chain)
  "."
  ","
] @punctuation.delimiter

[
  "-"
  "--"
  "-="
  "+"
  "++"
  "+="
  "*"
  "*="
  "**"
  "**="
  "/"
  "/="
  "%"
  "%="
  "<"
  "<="
  "<<"
  "<<="
  "="
  "=="
  "==="
  "!"
  "!="
  "!=="
  "=>"
  ">"
  ">="
  ">>"
  ">>="
  ">>>"
  ">>>="
  "~"
  "^"
  "&"
  "|"
  "^="
  "&="
  "|="
  "&&"
  "||"
  "??"
  "&&="
  "||="
  "??="
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
]  @punctuation.bracket

(template_substitution
  "${" @punctuation.special
  "}" @punctuation.special) @embedded

[
  "as"
  "async"
  "await"
  "break"
  "case"
  "catch"
  "class"
  "const"
  "continue"
  "debugger"
  "default"
  "delete"
  "do"
  "else"
  "export"
  "extends"
  "finally"
  "for"
  "from"
  "function"
  "get"
  "if"
  "import"
  "in"
  "instanceof"
  "let"
  "new"
  "of"
  "return"
  "set"
  "static"
  "switch"
  "target"
  "throw"
  "try"
  "typeof"
  "var"
  "void"
  "while"
  "with"
  "yield"
] @keyword
)TSQ";

static const char *kHighlightsBash = R"TSQ(
[
  (string)
  (raw_string)
  (heredoc_body)
  (heredoc_start)
] @string

(command_name) @function

(variable_name) @property

[
  "case"
  "do"
  "done"
  "elif"
  "else"
  "esac"
  "export"
  "fi"
  "for"
  "function"
  "if"
  "in"
  "select"
  "then"
  "unset"
  "until"
  "while"
] @keyword

(comment) @comment

(function_definition name: (word) @function)

(file_descriptor) @number

[
  (command_substitution)
  (process_substitution)
  (expansion)
]@embedded

[
  "$"
  "&&"
  ">"
  ">>"
  "<"
  "|"
] @operator

(
  (command (_) @constant)
  (#match? @constant "^-")
)
)TSQ";

static const char *kHighlightsCSharp = R"TSQ(
(identifier) @variable

;; Methods

(method_declaration name: (identifier) @function)
(local_function_statement name: (identifier) @function)

;; Types

(interface_declaration name: (identifier) @type)
(class_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(struct_declaration (identifier) @type)
(record_declaration (identifier) @type)
(namespace_declaration name: (identifier) @module)

(generic_name (identifier) @type)
(type_parameter (identifier) @property.definition)
(parameter type: (identifier) @type)
(type_argument_list (identifier) @type)
(as_expression right: (identifier) @type)
(is_expression right: (identifier) @type)

(constructor_declaration name: (identifier) @constructor)
(destructor_declaration name: (identifier) @constructor)

(_ type: (identifier) @type)

(base_list (identifier) @type)

(predefined_type) @type.builtin

;; Enum
(enum_member_declaration (identifier) @property.definition)

;; Literals

[
  (real_literal)
  (integer_literal)
] @number

[
  (character_literal)
  (string_literal)
  (raw_string_literal)
  (verbatim_string_literal)
  (interpolated_string_expression)
  (interpolation_start)
  (interpolation_quote)
 ] @string

(escape_sequence) @string.escape

[
  (boolean_literal)
  (null_literal)
] @constant.builtin

;; Comments

(comment) @comment

;; Tokens

[
  ";"
  "."
  ","
] @punctuation.delimiter

[
  "--"
  "-"
  "-="
  "&"
  "&="
  "&&"
  "+"
  "++"
  "+="
  "<"
  "<="
  "<<"
  "<<="
  "="
  "=="
  "!"
  "!="
  "=>"
  ">"
  ">="
  ">>"
  ">>="
  ">>>"
  ">>>="
  "|"
  "|="
  "||"
  "?"
  "??"
  "??="
  "^"
  "^="
  "~"
  "*"
  "*="
  "/"
  "/="
  "%"
  "%="
  ":"
  ".."
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  (interpolation_brace)
]  @punctuation.bracket

;; Keywords

[
  (modifier)
  "this"
  (implicit_type)
] @keyword

[
  "add"
  "alias"
  "as"
  "base"
  "break"
  "case"
  "catch"
  "checked"
  "class"
  "continue"
  "default"
  "delegate"
  "do"
  "else"
  "enum"
  "event"
  "explicit"
  "extern"
  "finally"
  "for"
  "foreach"
  "global"
  "goto"
  "if"
  "implicit"
  "interface"
  "is"
  "lock"
  "namespace"
  "notnull"
  "operator"
  "params"
  "return"
  "remove"
  "sizeof"
  "stackalloc"
  "static"
  "struct"
  "switch"
  "throw"
  "try"
  "typeof"
  "unchecked"
  "using"
  "while"
  "new"
  "await"
  "in"
  "yield"
  "get"
  "set"
  "when"
  "out"
  "ref"
  "from"
  "where"
  "select"
  "record"
  "init"
  "with"
  "let"
] @keyword

;; Attribute

(attribute name: (identifier) @attribute)

;; Parameters

(parameter
  name: (identifier) @variable.parameter)

;; Type constraints

(type_parameter_constraints_clause (identifier) @property.definition)

;; Method calls

(invocation_expression (member_access_expression name: (identifier) @function))
)TSQ";

static const char *kHighlightsCss = R"TSQ(
(comment) @comment

(tag_name) @tag
(nesting_selector) @tag
(universal_selector) @tag

"~" @operator
">" @operator
"+" @operator
"-" @operator
"*" @operator
"/" @operator
"=" @operator
"^=" @operator
"|=" @operator
"~=" @operator
"$=" @operator
"*=" @operator

"and" @operator
"or" @operator
"not" @operator
"only" @operator

(attribute_selector (plain_value) @string)

((property_name) @variable
 (#match? @variable "^--"))
((plain_value) @variable
 (#match? @variable "^--"))

(class_name) @property
(id_name) @property
(namespace_name) @property
(property_name) @property
(feature_name) @property

(pseudo_element_selector (tag_name) @attribute)
(pseudo_class_selector (class_name) @attribute)
(attribute_name) @attribute

(function_name) @function

"@media" @keyword
"@import" @keyword
"@charset" @keyword
"@namespace" @keyword
"@supports" @keyword
"@keyframes" @keyword
(at_keyword) @keyword
(to) @keyword
(from) @keyword
(important) @keyword

(string_value) @string
(color_value) @string.special

(integer_value) @number
(float_value) @number
(unit) @type

[
  "#"
  ","
  "."
  ":"
  "::"
  ";"
] @punctuation.delimiter

[
  "{"
  ")"
  "("
  "}"
] @punctuation.bracket
)TSQ";

static const char *kHighlightsGo = R"TSQ(
; Function calls

(call_expression
  function: (identifier) @function)

(call_expression
  function: (identifier) @function.builtin
  (#match? @function.builtin "^(append|cap|close|complex|copy|delete|imag|len|make|new|panic|print|println|real|recover)$"))

(call_expression
  function: (selector_expression
    field: (field_identifier) @function.method))

; Function definitions

(function_declaration
  name: (identifier) @function)

(method_declaration
  name: (field_identifier) @function.method)

; Identifiers

(type_identifier) @type
(field_identifier) @property
(identifier) @variable

; Operators

[
  "--"
  "-"
  "-="
  ":="
  "!"
  "!="
  "..."
  "*"
  "*"
  "*="
  "/"
  "/="
  "&"
  "&&"
  "&="
  "%"
  "%="
  "^"
  "^="
  "+"
  "++"
  "+="
  "<-"
  "<"
  "<<"
  "<<="
  "<="
  "="
  "=="
  ">"
  ">="
  ">>"
  ">>="
  "|"
  "|="
  "||"
  "~"
] @operator

; Keywords

[
  "break"
  "case"
  "chan"
  "const"
  "continue"
  "default"
  "defer"
  "else"
  "fallthrough"
  "for"
  "func"
  "go"
  "goto"
  "if"
  "import"
  "interface"
  "map"
  "package"
  "range"
  "return"
  "select"
  "struct"
  "switch"
  "type"
  "var"
] @keyword

; Literals

[
  (interpreted_string_literal)
  (raw_string_literal)
  (rune_literal)
] @string

(escape_sequence) @escape

[
  (int_literal)
  (float_literal)
  (imaginary_literal)
] @number

[
  (true)
  (false)
  (nil)
  (iota)
] @constant.builtin

(comment) @comment
)TSQ";

static const char *kHighlightsHaskell = R"TSQ(
; ----------------------------------------------------------------------------
; Parameters and variables
; NOTE: These are at the top, so that they have low priority,
; and don't override destructured parameters
(variable) @variable

(pattern/wildcard) @variable

(decl/function
  patterns: (patterns
    (_) @variable.parameter))

(expression/lambda
  (_)+ @variable.parameter
  "->")

(decl/function
  (infix
    (pattern) @variable.parameter))

; ----------------------------------------------------------------------------
; Literals and comments
(integer) @number

(negation) @number

(expression/literal
  (float)) @number.float

(char) @character

(string) @string

(unit) @string.special.symbol ; unit, as in ()

(comment) @comment

((haddock) @comment.documentation)

; ----------------------------------------------------------------------------
; Punctuation
[
  "("
  ")"
  "{"
  "}"
  "["
  "]"
] @punctuation.bracket

[
  ","
  ";"
] @punctuation.delimiter

; ----------------------------------------------------------------------------
; Keywords, operators, includes
[
  "forall"
  ; "∀" ; utf-8 is not cross-platform safe
] @keyword.repeat

(pragma) @keyword.directive

[
  "if"
  "then"
  "else"
  "case"
  "of"
] @keyword.conditional

[
  "import"
  "qualified"
  "module"
] @keyword.import

[
  (operator)
  (constructor_operator)
  (all_names)
  (wildcard)
  "."
  ".."
  "="
  "|"
  "::"
  "=>"
  "->"
  "<-"
  "\\"
  "`"
  "@"
] @operator

; TODO broken, also huh?
; ((qualified_module
;   (module) @constructor)
;   .
;   (module))

(module
  (module_id) @module)

[
  "where"
  "let"
  "in"
  "class"
  "instance"
  "pattern"
  "data"
  "newtype"
  "family"
  "type"
  "as"
  "hiding"
  "deriving"
  "via"
  "stock"
  "anyclass"
  "do"
  "mdo"
  "rec"
  "infix"
  "infixl"
  "infixr"
] @keyword

; ----------------------------------------------------------------------------
; Functions and variables
(decl
  [
   name: (variable) @function
   names: (binding_list (variable) @function)
  ])

(decl/bind
  name: (variable) @variable)

; Consider signatures (and accompanying functions)
; with only one value on the rhs as variables
(decl/signature
  name: (variable) @variable
  type: (type))

((decl/signature
  name: (variable) @_name
  type: (type))
  .
  (decl
    name: (variable) @variable)
    match: (_)
  (#eq? @_name @variable))

; but consider a type that involves 'IO' a decl/function
(decl/signature
  name: (variable) @function
  type: (type/apply
    constructor: (name) @_type)
  (#eq? @_type "IO"))

((decl/signature
  name: (variable) @_name
  type: (type/apply
    constructor: (name) @_type)
  (#eq? @_type "IO"))
  .
  (decl
    name: (variable) @function)
    match: (_)
  (#eq? @_name @function))

((decl/signature) @function
  .
  (decl/function
    name: (variable) @function))

(decl/bind
  name: (variable) @function
  (match
    expression: (expression/lambda)))

; view patterns
(view_pattern
  [
    (expression/variable) @function.call
    (expression/qualified
      (variable) @function.call)
  ])

; consider infix functions as operators
(infix_id
  [
    (variable) @operator
    (qualified
      (variable) @operator)
  ])

; decl/function calls with an infix operator
; e.g. func <$> a <*> b
(infix
  [
    (variable) @function.call
    (qualified
      ((module) @module
        (variable) @function.call))
  ]
  .
  (operator))

; infix operators applied to variables
((expression/variable) @variable
  .
  (operator))

((operator)
  .
  [
    (expression/variable) @variable
    (expression/qualified
      (variable) @variable)
  ])

; decl/function calls with infix operators
([
    (expression/variable) @function.call
    (expression/qualified
      (variable) @function.call)
  ]
  .
  (operator) @_op
  (#any-of? @_op "$" "<$>" ">>=" "=<<"))

; right hand side of infix operator
((infix
  [
    (operator)
    (infix_id (variable))
  ] ; infix or `func`
  .
  [
    (variable) @function.call
    (qualified
      (variable) @function.call)
  ])
  .
  (operator) @_op
  (#any-of? @_op "$" "<$>" "=<<"))

; decl/function composition, arrows, monadic composition (lhs)
(
  [
    (expression/variable) @function
    (expression/qualified
      (variable) @function)
  ]
  .
  (operator) @_op
  (#any-of? @_op "." ">>>" "***" ">=>" "<=<"))

; right hand side of infix operator
((infix
  [
    (operator)
    (infix_id (variable))
  ] ; infix or `func`
  .
  [
    (variable) @function
    (qualified
      (variable) @function)
  ])
  .
  (operator) @_op
  (#any-of? @_op "." ">>>" "***" ">=>" "<=<"))

; function composition, arrows, monadic composition (rhs)
((operator) @_op
  .
  [
    (expression/variable) @function
    (expression/qualified
      (variable) @function)
  ]
  (#any-of? @_op "." ">>>" "***" ">=>" "<=<"))

; function defined in terms of a function composition
(decl/function
  name: (variable) @function
  (match
    expression: (infix
      operator: (operator) @_op
      (#any-of? @_op "." ">>>" "***" ">=>" "<=<"))))

(apply
  [
    (expression/variable) @function.call
    (expression/qualified
      (variable) @function.call)
  ])

; function compositions, in parentheses, applied
; lhs
(apply
  .
  (expression/parens
    (infix
      [
        (variable) @function.call
        (qualified
          (variable) @function.call)
      ]
      .
      (operator))))

; rhs
(apply
  .
  (expression/parens
    (infix
      (operator)
      .
      [
        (variable) @function.call
        (qualified
          (variable) @function.call)
      ])))

; variables being passed to a function call
(apply
  (_)
  .
  [
    (expression/variable) @variable
    (expression/qualified
      (variable) @variable)
  ])

; main is always a function
; (this prevents `main = undefined` from being highlighted as a variable)
(decl/bind
  name: (variable) @function
  (#eq? @function "main"))

; scoped function types (func :: a -> b)
(signature
  pattern: (pattern/variable) @function
  type: (quantified_type))

; signatures that have a function type
; + binds that follow them
(decl/signature
  name: (variable) @function
  type: (quantified_type))

((decl/signature
  name: (variable) @_name
  type: (quantified_type))
  .
  (decl/bind
    (variable) @function)
  (#eq? @function @_name))

; ----------------------------------------------------------------------------
; Types
(name) @type

(type/star) @type

(variable) @type

(constructor) @constructor

; True or False
((constructor) @boolean
  (#any-of? @boolean "True" "False"))

; otherwise (= True)
((variable) @boolean
  (#eq? @boolean "otherwise"))

; ----------------------------------------------------------------------------
; Quasi-quotes
(quoter) @function.call

(quasiquote
  [
    (quoter) @_name
    (_
      (variable) @_name)
  ]
  (#eq? @_name "qq")
  (quasiquote_body) @string)

(quasiquote
  (_
    (variable) @_name)
  (#eq? @_name "qq")
  (quasiquote_body) @string)

; namespaced quasi-quoter
(quasiquote
  (_
    (module) @module
    .
    (variable) @function.call))

; Highlighting of quasiquote_body for other languages is handled by injections.scm
; ----------------------------------------------------------------------------
; Exceptions/error handling
((variable) @keyword.exception
  (#any-of? @keyword.exception
    "error" "undefined" "try" "tryJust" "tryAny" "catch" "catches" "catchJust" "handle" "handleJust"
    "throw" "throwIO" "throwTo" "throwError" "ioError" "mask" "mask_" "uninterruptibleMask"
    "uninterruptibleMask_" "bracket" "bracket_" "bracketOnErrorSource" "finally" "fail"
    "onException" "expectationFailure"))

; ----------------------------------------------------------------------------
; Debugging
((variable) @keyword.debug
  (#any-of? @keyword.debug
    "trace" "traceId" "traceShow" "traceShowId" "traceWith" "traceShowWith" "traceStack" "traceIO"
    "traceM" "traceShowM" "traceEvent" "traceEventWith" "traceEventIO" "flushEventLog" "traceMarker"
    "traceMarkerIO"))

; ----------------------------------------------------------------------------
; Fields

(field_name
  (variable) @variable.member)

(import_name
  (name)
  .
  (children
    (variable) @variable.member))


; ----------------------------------------------------------------------------
; Spell checking
(comment) @spell
)TSQ";

static const char *kHighlightsHtml = R"TSQ(
(tag_name) @tag
(erroneous_end_tag_name) @tag.error
(doctype) @constant
(attribute_name) @attribute
(attribute_value) @string
(comment) @comment

[
  "<"
  ">"
  "</"
  "/>"
] @punctuation.bracket
)TSQ";

static const char *kHighlightsJava = R"TSQ(
; Variables

(identifier) @variable

; Methods

(method_declaration
  name: (identifier) @function.method)
(method_invocation
  name: (identifier) @function.method)
(super) @function.builtin

; Annotations

(annotation
  name: (identifier) @attribute)
(marker_annotation
  name: (identifier) @attribute)

"@" @operator

; Types

(type_identifier) @type

(interface_declaration
  name: (identifier) @type)
(class_declaration
  name: (identifier) @type)
(enum_declaration
  name: (identifier) @type)

((field_access
  object: (identifier) @type)
 (#match? @type "^[A-Z]"))
((scoped_identifier
  scope: (identifier) @type)
 (#match? @type "^[A-Z]"))
((method_invocation
  object: (identifier) @type)
 (#match? @type "^[A-Z]"))
((method_reference
  . (identifier) @type)
 (#match? @type "^[A-Z]"))

(constructor_declaration
  name: (identifier) @type)

[
  (boolean_type)
  (integral_type)
  (floating_point_type)
  (floating_point_type)
  (void_type)
] @type.builtin

; Constants

((identifier) @constant
 (#match? @constant "^_*[A-Z][A-Z\\d_]+$"))

; Builtins

(this) @variable.builtin

; Literals

[
  (hex_integer_literal)
  (decimal_integer_literal)
  (octal_integer_literal)
  (decimal_floating_point_literal)
  (hex_floating_point_literal)
] @number

[
  (character_literal)
  (string_literal)
] @string
(escape_sequence) @string.escape

[
  (true)
  (false)
  (null_literal)
] @constant.builtin

[
  (line_comment)
  (block_comment)
] @comment

; Keywords

[
  "abstract"
  "assert"
  "break"
  "case"
  "catch"
  "class"
  "continue"
  "default"
  "do"
  "else"
  "enum"
  "exports"
  "extends"
  "final"
  "finally"
  "for"
  "if"
  "implements"
  "import"
  "instanceof"
  "interface"
  "module"
  "native"
  "new"
  "non-sealed"
  "open"
  "opens"
  "package"
  "permits"
  "private"
  "protected"
  "provides"
  "public"
  "requires"
  "record"
  "return"
  "sealed"
  "static"
  "strictfp"
  "switch"
  "synchronized"
  "throw"
  "throws"
  "to"
  "transient"
  "transitive"
  "try"
  "uses"
  "volatile"
  "when"
  "while"
  "with"
  "yield"
] @keyword
)TSQ";

static const char *kHighlightsJson = R"TSQ(
(pair
  key: (_) @string.special.key)

(string) @string

(number) @number

[
  (null)
  (true)
  (false)
] @constant.builtin

(escape_sequence) @escape

(comment) @comment
)TSQ";

static const char *kHighlightsJulia = R"TSQ(
; Identifiers
(identifier) @variable

(field_expression
  (identifier) @variable.member .)

; Symbols
(quote_expression
  ":" @string.special.symbol
  [
    (identifier)
    (operator)
  ] @string.special.symbol)

; Function calls
(call_expression
  (identifier) @function.call)

(call_expression
  (field_expression
    (identifier) @function.call .))

(broadcast_call_expression
  (identifier) @function.call)

(broadcast_call_expression
  (field_expression
    (identifier) @function.call .))

(binary_expression
  (_)
  (operator) @_pipe
  (identifier) @function.call
  (#any-of? @_pipe "|>" ".|>"))

; Macros
(macro_identifier
  "@" @function.macro
  (identifier) @function.macro)

(macro_definition
  (signature
    (call_expression
      .
      (identifier) @function.macro)))

; Built-in functions
; print.("\"", filter(name -> getglobal(Core, name) isa Core.Builtin, names(Core)), "\" ")
((identifier) @function.builtin
  (#any-of? @function.builtin
    "applicable" "fieldtype" "getfield" "getglobal" "invoke" "isa" "isdefined" "isdefinedglobal"
    "modifyfield!" "modifyglobal!" "nfields" "replacefield!" "replaceglobal!" "setfield!"
    "setfieldonce!" "setglobal!" "setglobalonce!" "swapfield!" "swapglobal!" "throw" "tuple"
    "typeassert" "typeof"))

; Type definitions
(type_head
  (_) @type.definition)

; Type annotations
(parametrized_type_expression
  [
    (identifier) @type
    (field_expression
      (identifier) @type .)
  ]
  (curly_expression
    (_) @type))

(typed_expression
  (identifier) @type .)

(unary_typed_expression
  (identifier) @type .)

(where_expression
  [
    (curly_expression
      (_) @type)
    (_) @type
  ] .)

(unary_expression
  (operator) @operator
  (_) @type
  (#any-of? @operator "<:" ">:"))

(binary_expression
  (_) @type
  (operator) @operator
  (_) @type
  (#any-of? @operator "<:" ">:"))

; Built-in types
; print.("\"", filter(name -> typeof(Base.eval(Core, name)) in [DataType, UnionAll], names(Core)), "\" ")
((identifier) @type.builtin
  (#any-of? @type.builtin
    "AbstractArray" "AbstractChar" "AbstractFloat" "AbstractString" "Any" "ArgumentError" "Array"
    "AssertionError" "AtomicMemory" "AtomicMemoryRef" "Bool" "BoundsError" "Char"
    "ConcurrencyViolationError" "Cvoid" "DataType" "DenseArray" "DivideError" "DomainError"
    "ErrorException" "Exception" "Expr" "FieldError" "Float16" "Float32" "Float64" "Function"
    "GenericMemory" "GenericMemoryRef" "GlobalRef" "IO" "InexactError" "InitError" "Int" "Int128"
    "Int16" "Int32" "Int64" "Int8" "Integer" "InterruptException" "LineNumberNode" "LoadError"
    "Memory" "MemoryRef" "Method" "MethodError" "Module" "NTuple" "NamedTuple" "Nothing" "Number"
    "OutOfMemoryError" "OverflowError" "Pair" "Ptr" "QuoteNode" "ReadOnlyMemoryError" "Real" "Ref"
    "SegmentationFault" "Signed" "StackOverflowError" "String" "Symbol" "Task" "Tuple" "Type"
    "TypeError" "TypeVar" "UInt" "UInt128" "UInt16" "UInt32" "UInt64" "UInt8" "UndefInitializer"
    "UndefKeywordError" "UndefRefError" "UndefVarError" "Union" "UnionAll" "Unsigned" "VecElement"
    "WeakRef"))

; Keywords
[
  "global"
  "local"
] @keyword

(compound_statement
  [
    "begin"
    "end"
  ] @keyword)

(quote_statement
  [
    "quote"
    "end"
  ] @keyword)

(let_statement
  [
    "let"
    "end"
  ] @keyword)

(if_statement
  [
    "if"
    "end"
  ] @keyword.conditional)

(elseif_clause
  "elseif" @keyword.conditional)

(else_clause
  "else" @keyword.conditional)

(ternary_expression
  [
    "?"
    ":"
  ] @keyword.conditional.ternary)

(try_statement
  [
    "try"
    "end"
  ] @keyword.exception)

(catch_clause
  "catch" @keyword.exception)

(finally_clause
  "finally" @keyword.exception)

(for_statement
  [
    "for"
    "end"
  ] @keyword.repeat)

(for_binding
  "outer" @keyword.repeat)

; comprehensions
(for_clause
  "for" @keyword.repeat)

(if_clause
  "if" @keyword.conditional)

(while_statement
  [
    "while"
    "end"
  ] @keyword.repeat)

[
  (break_statement)
  (continue_statement)
] @keyword.repeat

[
  "const"
  "mutable"
] @keyword.modifier

(function_definition
  [
    "function"
    "end"
  ] @keyword.function)

(do_clause
  [
    "do"
    "end"
  ] @keyword.function)

(macro_definition
  [
    "macro"
    "end"
  ] @keyword)

(return_statement
  "return" @keyword.return)

(module_definition
  [
    "module"
    "baremodule"
    "end"
  ] @keyword.import)

(export_statement
  "export" @keyword.import)

(public_statement
  "public" @keyword.import)

(import_statement
  "import" @keyword.import)

(using_statement
  "using" @keyword.import)

(import_alias
  "as" @keyword.import)

(selected_import
  ":" @punctuation.delimiter)

(struct_definition
  [
    "mutable"
    "struct"
    "end"
  ] @keyword.type)

(abstract_definition
  [
    "abstract"
    "type"
    "end"
  ] @keyword.type)

(primitive_definition
  [
    "primitive"
    "type"
    "end"
  ] @keyword.type)

; Operators & Punctuation
(operator) @operator

(adjoint_expression
  "'" @operator)

(range_expression
  ":" @operator)

(arrow_function_expression
  "->" @operator)

[
  "."
  "..."
] @punctuation.special

[
  ","
  ";"
  "::"
] @punctuation.delimiter

; Treat `::` as operator in type contexts, see
; https://github.com/nvim-treesitter/nvim-treesitter/pull/7392
(typed_expression
  "::" @operator)

(unary_typed_expression
  "::" @operator)

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

; Interpolation
(string_interpolation
  .
  "$" @punctuation.special)

(interpolation_expression
  .
  "$" @punctuation.special)

; Keyword operators
((operator) @keyword.operator
  (#any-of? @keyword.operator "in" "isa"))

(where_expression
  "where" @keyword.operator)

; Built-in constants
((identifier) @constant.builtin
  (#any-of? @constant.builtin "nothing" "missing"))

((identifier) @variable.builtin
  (#any-of? @variable.builtin "begin" "end")
  (#has-ancestor? @variable.builtin index_expression))

; Literals
(boolean_literal) @boolean

(integer_literal) @number

(float_literal) @number.float

((identifier) @number.float
  (#any-of? @number.float "NaN" "NaN16" "NaN32" "Inf" "Inf16" "Inf32"))

(character_literal) @character

(escape_sequence) @string.escape

(string_literal) @string

(prefixed_string_literal
  prefix: (identifier) @function.macro) @string

(command_literal) @string.special

(prefixed_command_literal
  prefix: (identifier) @function.macro) @string.special

((string_literal) @string.documentation
  .
  [
    (abstract_definition)
    (assignment)
    (const_statement)
    (function_definition)
    (macro_definition)
    (module_definition)
    (struct_definition)
  ])

(source_file
  (string_literal) @string.documentation
  .
  [
    (identifier)
    (call_expression)
  ])

[
  (line_comment)
  (block_comment)
] @comment
)TSQ";

static const char *kHighlightsRuby = R"TSQ(
(identifier) @variable

((identifier) @function.method
 (#is-not? local))

[
  "alias"
  "and"
  "begin"
  "break"
  "case"
  "class"
  "def"
  "do"
  "else"
  "elsif"
  "end"
  "ensure"
  "for"
  "if"
  "in"
  "module"
  "next"
  "or"
  "rescue"
  "retry"
  "return"
  "then"
  "unless"
  "until"
  "when"
  "while"
  "yield"
] @keyword

((identifier) @keyword
 (#match? @keyword "^(private|protected|public)$"))

(constant) @constructor

; Function calls

"defined?" @function.method.builtin

(call
  method: [(identifier) (constant)] @function.method)

((identifier) @function.method.builtin
 (#eq? @function.method.builtin "require"))

; Function definitions

(alias (identifier) @function.method)
(setter (identifier) @function.method)
(method name: [(identifier) (constant)] @function.method)
(singleton_method name: [(identifier) (constant)] @function.method)

; Identifiers

[
  (class_variable)
  (instance_variable)
] @property

((identifier) @constant.builtin
 (#match? @constant.builtin "^__(FILE|LINE|ENCODING)__$"))

(file) @constant.builtin
(line) @constant.builtin
(encoding) @constant.builtin

(hash_splat_nil
  "**" @operator) @constant.builtin

((constant) @constant
 (#match? @constant "^[A-Z\\d_]+$"))

[
  (self)
  (super)
] @variable.builtin

(block_parameter (identifier) @variable.parameter)
(block_parameters (identifier) @variable.parameter)
(destructured_parameter (identifier) @variable.parameter)
(hash_splat_parameter (identifier) @variable.parameter)
(lambda_parameters (identifier) @variable.parameter)
(method_parameters (identifier) @variable.parameter)
(splat_parameter (identifier) @variable.parameter)

(keyword_parameter name: (identifier) @variable.parameter)
(optional_parameter name: (identifier) @variable.parameter)

; Literals

[
  (string)
  (bare_string)
  (subshell)
  (heredoc_body)
  (heredoc_beginning)
] @string

[
  (simple_symbol)
  (delimited_symbol)
  (hash_key_symbol)
  (bare_symbol)
] @string.special.symbol

(regex) @string.special.regex
(escape_sequence) @escape

[
  (integer)
  (float)
] @number

[
  (nil)
  (true)
  (false)
] @constant.builtin

(interpolation
  "#{" @punctuation.special
  "}" @punctuation.special) @embedded

(comment) @comment

; Operators

[
"="
"=>"
"->"
] @operator

[
  ","
  ";"
  "."
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  "%w("
  "%i("
] @punctuation.bracket
)TSQ";

static const char *kHighlightsRust = R"TSQ(
; Identifiers

(type_identifier) @type
(primitive_type) @type.builtin
(field_identifier) @property

; Identifier conventions

; Assume all-caps names are constants
((identifier) @constant
 (#match? @constant "^[A-Z][A-Z\\d_]+$'"))

; Assume uppercase names are enum constructors
((identifier) @constructor
 (#match? @constructor "^[A-Z]"))

; Assume that uppercase names in paths are types
((scoped_identifier
  path: (identifier) @type)
 (#match? @type "^[A-Z]"))
((scoped_identifier
  path: (scoped_identifier
    name: (identifier) @type))
 (#match? @type "^[A-Z]"))
((scoped_type_identifier
  path: (identifier) @type)
 (#match? @type "^[A-Z]"))
((scoped_type_identifier
  path: (scoped_identifier
    name: (identifier) @type))
 (#match? @type "^[A-Z]"))

; Assume all qualified names in struct patterns are enum constructors. (They're
; either that, or struct names; highlighting both as constructors seems to be
; the less glaring choice of error, visually.)
(struct_pattern
  type: (scoped_type_identifier
    name: (type_identifier) @constructor))

; Function calls

(call_expression
  function: (identifier) @function)
(call_expression
  function: (field_expression
    field: (field_identifier) @function.method))
(call_expression
  function: (scoped_identifier
    "::"
    name: (identifier) @function))

(generic_function
  function: (identifier) @function)
(generic_function
  function: (scoped_identifier
    name: (identifier) @function))
(generic_function
  function: (field_expression
    field: (field_identifier) @function.method))

(macro_invocation
  macro: (identifier) @function.macro
  "!" @function.macro)

; Function definitions

(function_item (identifier) @function)
(function_signature_item (identifier) @function)

(line_comment) @comment
(block_comment) @comment

(line_comment (doc_comment)) @comment.documentation
(block_comment (doc_comment)) @comment.documentation

"(" @punctuation.bracket
")" @punctuation.bracket
"[" @punctuation.bracket
"]" @punctuation.bracket
"{" @punctuation.bracket
"}" @punctuation.bracket

(type_arguments
  "<" @punctuation.bracket
  ">" @punctuation.bracket)
(type_parameters
  "<" @punctuation.bracket
  ">" @punctuation.bracket)

"::" @punctuation.delimiter
":" @punctuation.delimiter
"." @punctuation.delimiter
"," @punctuation.delimiter
";" @punctuation.delimiter

(parameter (identifier) @variable.parameter)

(lifetime (identifier) @label)

"as" @keyword
"async" @keyword
"await" @keyword
"break" @keyword
"const" @keyword
"continue" @keyword
"default" @keyword
"dyn" @keyword
"else" @keyword
"enum" @keyword
"extern" @keyword
"fn" @keyword
"for" @keyword
"gen" @keyword
"if" @keyword
"impl" @keyword
"in" @keyword
"let" @keyword
"loop" @keyword
"macro_rules!" @keyword
"match" @keyword
"mod" @keyword
"move" @keyword
"pub" @keyword
"raw" @keyword
"ref" @keyword
"return" @keyword
"static" @keyword
"struct" @keyword
"trait" @keyword
"type" @keyword
"union" @keyword
"unsafe" @keyword
"use" @keyword
"where" @keyword
"while" @keyword
"yield" @keyword
(crate) @keyword
(mutable_specifier) @keyword
(use_list (self) @keyword)
(scoped_use_list (self) @keyword)
(scoped_identifier (self) @keyword)
(super) @keyword

(self) @variable.builtin

(char_literal) @string
(string_literal) @string
(raw_string_literal) @string

(boolean_literal) @constant.builtin
(integer_literal) @constant.builtin
(float_literal) @constant.builtin

(escape_sequence) @escape

(attribute_item) @attribute
(inner_attribute_item) @attribute

"*" @operator
"&" @operator
"'" @operator
)TSQ";

// tree-sitter-scala 0.26.0's own queries/highlights.scm (flake.nix's
// pinned grammar): the copy vendored before named a "forSome" token that
// version dropped, so ts_query_new rejected the whole query.
static const char *kHighlightsScala = R"TSQ(
; CREDITS @stumash (stuart.mashaal@gmail.com)

(field_expression field: (identifier) @property)
(field_expression value: (identifier) @type
 (#match? @type "^[A-Z]"))

(type_identifier) @type

(class_definition
  name: (identifier) @type)

(enum_definition
  name: (identifier) @type)

(object_definition
  name: (identifier) @type)

(trait_definition
  name: (identifier) @type)

(full_enum_case
  name: (identifier) @type)

(simple_enum_case
  name: (identifier) @type)

;; variables

(class_parameter
  name: (identifier) @parameter)

(self_type (identifier) @parameter)

(interpolation (identifier) @none)
(interpolation (block) @none)

;; types

(type_definition
  name: (type_identifier) @type.definition)

;; val/var definitions/declarations

(val_definition
  pattern: (identifier) @variable)

(var_definition
  pattern: (identifier) @variable)

(val_declaration
  name: (identifier) @variable)

(var_declaration
  name: (identifier) @variable)

; imports/exports

(import_declaration
  path: (identifier) @namespace)
((stable_identifier (identifier) @namespace))

((import_declaration
  path: (identifier) @type) (#match? @type "^[A-Z]"))
((stable_identifier (identifier) @type) (#match? @type "^[A-Z]"))

(export_declaration
  path: (identifier) @namespace)
((stable_identifier (identifier) @namespace))

((export_declaration
  path: (identifier) @type) (#match? @type "^[A-Z]"))
((stable_identifier (identifier) @type) (#match? @type "^[A-Z]"))

((namespace_selectors (identifier) @type) (#match? @type "^[A-Z]"))

; method invocation

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (operator_identifier) @function.call)

(call_expression
  function: (field_expression
    field: (identifier) @method.call))

((call_expression
   function: (identifier) @constructor)
 (#match? @constructor "^[A-Z]"))

(generic_function
  function: (identifier) @function.call)

(interpolated_string_expression
  interpolator: (identifier) @function.call)

; function definitions

(function_definition
  name: (identifier) @function)

(parameter
  name: (identifier) @parameter)

(binding
  name: (identifier) @parameter)

; method definition

(function_declaration
      name: (identifier) @method)

(function_definition
      name: (identifier) @method)

; expressions

(infix_expression operator: (identifier) @operator)
(infix_expression operator: (operator_identifier) @operator)
(infix_type operator: (operator_identifier) @operator)
(infix_type operator: (operator_identifier) @operator)

; literals

(boolean_literal) @boolean
(integer_literal) @number
(floating_point_literal) @float

[
  (string)
  (character_literal)
  (interpolated_string_expression)
] @string

(interpolation "$" @punctuation.special)

;; keywords

(opaque_modifier) @type.qualifier
(infix_modifier) @keyword
(transparent_modifier) @type.qualifier
(open_modifier) @type.qualifier

[
  "case"
  "class"
  "enum"
  "extends"
  "derives"
  "finally"
;; `forSome` existential types not implemented yet
;; `macro` not implemented yet
  "object"
  "override"
  "package"
  "trait"
  "type"
  "val"
  "var"
  "with"
  "given"
  "using"
  "end"
  "implicit"
  "extension"
  "with"
] @keyword

[
  "abstract"
  "final"
  "lazy"
  "sealed"
  "private"
  "protected"
] @type.qualifier

(inline_modifier) @storageclass

(null_literal) @constant.builtin

(wildcard) @parameter

(annotation) @attribute

;; special keywords

"new" @keyword.operator

[
  "else"
  "if"
  "match"
  "then"
] @conditional

[
 "("
 ")"
 "["
 "]"
 "{"
 "}"
]  @punctuation.bracket

[
 "."
 ","
] @punctuation.delimiter

[
  "do"
  "for"
  "while"
  "yield"
] @repeat

"def" @keyword.function

[
 "=>"
 "<-"
 "@"
] @operator

["import" "export"] @include

[
  "try"
  "catch"
  "throw"
] @exception

"return" @keyword.return

(comment) @comment
(block_comment) @comment

;; `case` is a conditional keyword in case_block

(case_block
  (case_clause ("case") @conditional))
(indented_cases
  (case_clause ("case") @conditional))

(operator_identifier) @operator

((identifier) @type (#match? @type "^[A-Z]"))
((identifier) @variable.builtin
 (#match? @variable.builtin "^this$"))

(
  (identifier) @function.builtin
  (#match? @function.builtin "^super$")
)

;; Scala CLI using directives
(using_directive_key) @parameter
(using_directive_value) @string
)TSQ";

static const char *kHighlightsOcaml = R"TSQ(
; Punctuation
;------------

[
  "," "." ";" ":" "=" "|" "~" "?" "+" "-" "!" ">" "&"
  "->" ";;" ":>" "+=" ":=" ".."
] @punctuation.delimiter

["(" ")" "[" "]" "{" "}" "[|" "|]" "[<" "[>"] @punctuation.bracket

(object_type ["<" ">"] @punctuation.bracket)

"%" @punctuation.special

(attribute ["[@" "]"] @punctuation.special)
(item_attribute ["[@@" "]"] @punctuation.special)
(floating_attribute ["[@@@" "]"] @punctuation.special)
(extension ["[%" "]"] @punctuation.special)
(item_extension ["[%%" "]"] @punctuation.special)
(quoted_extension ["{%" "}"] @punctuation.special)
(quoted_item_extension ["{%%" "}"] @punctuation.special)

; Keywords
;---------

[
  "and" "as" "assert" "begin" "class" "constraint" "do" "done" "downto" "effect"
  "else" "end" "exception" "external" "for" "fun" "function" "functor" "if" "in"
  "include" "inherit" "initializer" "lazy" "let" "match" "method" "module"
  "mutable" "new" "nonrec" "object" "of" "open" "private" "rec" "sig" "struct"
  "then" "to" "try" "type" "val" "virtual" "when" "while" "with"
] @keyword

; Operators
;----------

[
  (prefix_operator)
  (sign_operator)
  (pow_operator)
  (mult_operator)
  (add_operator)
  (concat_operator)
  (rel_operator)
  (and_operator)
  (or_operator)
  (assign_operator)
  (hash_operator)
  (indexing_operator)
  (let_operator)
  (let_and_operator)
  (match_operator)
] @operator

(match_expression (match_operator) @keyword)

(value_definition [(let_operator) (let_and_operator)] @keyword)

["*" "#" "::" "<-"] @operator

; Constants
;----------

(boolean) @constant

[(number) (signed_number)] @number

[(string) (character)] @string

(quoted_string "{" @string "}" @string) @string

(escape_sequence) @escape

(conversion_specification) @string.special

; Variables
;----------

[(value_name) (type_variable)] @variable

(value_pattern) @variable.parameter

; Properties
;-----------

[(label_name) (field_name) (instance_variable_name)] @property

; Functions
;----------

(let_binding
  pattern: (value_name) @function
  (parameter))

(let_binding
  pattern: (value_name) @function
  body: [(fun_expression) (function_expression)])

(value_specification (value_name) @function)

(external (value_name) @function)

(method_name) @function.method

(application_expression
  function: (value_path (value_name) @function))

(infix_expression
  left: (value_path (value_name) @function)
  operator: (concat_operator) @operator
  (#eq? @operator "@@"))

(infix_expression
  operator: (rel_operator) @operator
  right: (value_path (value_name) @function)
  (#eq? @operator "|>"))

(
  (value_name) @function.builtin
  (#match? @function.builtin "^(raise(_notrace)?|failwith|invalid_arg)$")
)

; Types
;------

[(class_name) (class_type_name) (type_constructor)] @type

(
  (type_constructor) @type.builtin
  (#match? @type.builtin "^(int|char|bytes|string|float|bool|unit|exn|array|list|option|int32|int64|nativeint|format6|lazy_t)$")
)

[(constructor_name) (tag)] @constructor

; Modules
;--------

[(module_name) (module_type_name)] @module

; Attributes
;-----------

(attribute_id) @tag

; Comments
;---------

[(comment) (line_number_directive) (directive) (shebang)] @comment
)TSQ";

static const char *kHighlightsPhp = R"TSQ(
[
  (php_tag)
  (php_end_tag)
] @tag

; Keywords

[
  "and"
  "as"
  "break"
  "case"
  "catch"
  "class"
  "clone"
  "const"
  "continue"
  "declare"
  "default"
  "do"
  "echo"
  "else"
  "elseif"
  "enddeclare"
  "endfor"
  "endforeach"
  "endif"
  "endswitch"
  "endwhile"
  "enum"
  "exit"
  "extends"
  "finally"
  "fn"
  "for"
  "foreach"
  "function"
  "global"
  "goto"
  "if"
  "implements"
  "include"
  "include_once"
  "instanceof"
  "insteadof"
  "interface"
  "match"
  "namespace"
  "new"
  "or"
  "print"
  "require"
  "require_once"
  "return"
  "switch"
  "throw"
  "trait"
  "try"
  "use"
  "while"
  "xor"
  "yield"
  "yield from"
  (abstract_modifier)
  (final_modifier)
  (readonly_modifier)
  (static_modifier)
  (visibility_modifier)
] @keyword

(function_static_declaration "static" @keyword)

; Namespace

(namespace_definition
  name: (namespace_name
    (name) @module))

(namespace_name
  (name) @module)

(namespace_use_clause
  [
    (name) @type
    (qualified_name
      (name) @type)
    alias: (name) @type
  ])

(namespace_use_clause
  type: "function"
  [
    (name) @function
    (qualified_name
      (name) @function)
    alias: (name) @function
  ])

(namespace_use_clause
  type: "const"
  [
    (name) @constant
    (qualified_name
      (name) @constant)
    alias: (name) @constant
  ])

(relative_name "namespace" @module.builtin)

; Variables

(relative_scope) @variable.builtin

(variable_name) @variable

(method_declaration name: (name) @constructor
  (#eq? @constructor "__construct"))

(object_creation_expression [
  (name) @constructor
  (qualified_name (name) @constructor)
  (relative_name (name) @constructor)
])

((name) @constant
 (#match? @constant "^_?[A-Z][A-Z\\d_]+$"))
((name) @constant.builtin
 (#match? @constant.builtin "^__[A-Z][A-Z\d_]+__$"))
(const_declaration (const_element (name) @constant))

; Types

(primitive_type) @type.builtin
(cast_type) @type.builtin
(named_type [
  (name) @type
  (qualified_name (name) @type)
  (relative_name (name) @type)
]) @type
(named_type (name) @type.builtin
  (#any-of? @type.builtin "static" "self"))

(scoped_call_expression
  scope: [
    (name) @type
    (qualified_name (name) @type)
    (relative_name (name) @type)
  ])

; Functions

(array_creation_expression "array" @function.builtin)
(list_literal "list" @function.builtin)
(exit_statement "exit" @function.builtin "(")

(method_declaration
  name: (name) @function.method)

(function_call_expression
  function: [
    (qualified_name (name))
    (relative_name (name))
    (name)
  ] @function)

(scoped_call_expression
  name: (name) @function)

(member_call_expression
  name: (name) @function.method)

(function_definition
  name: (name) @function)

; Member

(property_element
  (variable_name) @property)

(member_access_expression
  name: (variable_name (name)) @property)
(member_access_expression
  name: (name) @property)

; Basic tokens
[
  (string)
  (string_content)
  (encapsed_string)
  (heredoc)
  (heredoc_body)
  (nowdoc_body)
] @string
(boolean) @constant.builtin
(null) @constant.builtin
(integer) @number
(float) @number
(comment) @comment

((name) @variable.builtin
 (#eq? @variable.builtin "this"))

"$" @operator
)TSQ";

static const char *kHighlightsTypescript = R"TSQ(
; Types

(type_identifier) @type
(predefined_type) @type.builtin

((identifier) @type
 (#match? @type "^[A-Z]"))

(type_arguments
  "<" @punctuation.bracket
  ">" @punctuation.bracket)

; Variables

(required_parameter (identifier) @variable.parameter)
(optional_parameter (identifier) @variable.parameter)

; Keywords

[ "abstract"
  "declare"
  "enum"
  "export"
  "implements"
  "interface"
  "keyof"
  "namespace"
  "private"
  "protected"
  "public"
  "type"
  "readonly"
  "override"
  "satisfies"
] @keyword
)TSQ";

static const char *kHighlightsCsv = R"TSQ(
(text) @string
(number) @number
(float) @float
(boolean) @boolean
"," @punctuation.delimiter
)TSQ";

// tree-sitter-diff 0.1.0's own queries/highlights.scm (flake.nix's pinned
// grammar); the query vendored before was written for a newer grammar
// (change/binary_hunk nodes) and failed to compile against this one.
static const char *kHighlightsDiff = R"TSQ(
(comment) @comment

[
  (addition)
  (new_file)
] @diff.plus

[
  (deletion)
  (old_file)
] @diff.minus

(commit) @constant

(location) @attribute

(command
  "diff" @function
  (argument) @variable.parameter)

(filename) @string.special.path

(mode) @number

([
  ".."
  "+"
  "++"
  "+++"
  "++++"
  "-"
  "--"
  "---"
  "----"
] @punctuation.special
  (#set! priority 95))

[
  (binary_change)
  (similarity)
  (file_change)
] @label

(index
  "index" @keyword)

(similarity
  (score) @number
  "%" @number)
)TSQ";

static const char *kHighlightsMake = R"TSQ(
[
 "("
 ")"
 "{"
 "}"
] @punctuation.bracket

[
 ":"
 "&:"
 "::"
 "|"
 ";"
 "\""
 "'"
 ","
] @punctuation.delimiter

[
 "$"
 "$$"
] @punctuation.special

(automatic_variable
 [ "@" "%" "<" "?" "^" "+" "/" "*" "D" "F"] @punctuation.special)

(automatic_variable
 "/" @error . ["D" "F"])

[
 "="
 ":="
 "::="
 "?="
 "+="
 "!="
 "@"
 "-"
 "+"
] @operator

[
 (text)
 (string)
 (raw_text)
] @string

(variable_assignment (word) @string)

[
 "ifeq"
 "ifneq"
 "ifdef"
 "ifndef"
 "else"
 "endif"
 "if"
 "or"  ; boolean functions are conditional in make grammar
 "and"
] @conditional

"foreach" @repeat

[
 "define"
 "endef"
 "vpath"
 "undefine"
 "export"
 "unexport"
 "override"
 "private"
; "load"
] @keyword

[
 "include"
 "sinclude"
 "-include"
] @include

[
 "subst"
 "patsubst"
 "strip"
 "findstring"
 "filter"
 "filter-out"
 "sort"
 "word"
 "words"
 "wordlist"
 "firstword"
 "lastword"
 "dir"
 "notdir"
 "suffix"
 "basename"
 "addsuffix"
 "addprefix"
 "join"
 "wildcard"
 "realpath"
 "abspath"
 "call"
 "eval"
 "file"
 "value"
 "shell"
] @keyword.function

[
 "error"
 "warning"
 "info"
] @exception

;; Variable
(variable_assignment
  name: (word) @constant)

(variable_reference
  (word) @constant)

(comment) @comment

((word) @clean @string.regex
 (#match? @clean "[%\*\?]"))

(function_call
  function: "error"
  (arguments (text) @text.danger))

(function_call
  function: "warning"
  (arguments (text) @text.warning))

(function_call
  function: "info"
  (arguments (text) @text.note))

;; Install Command Categories
;; Others special variables
;; Variables Used by Implicit Rules
[
 "VPATH"
 ".RECIPEPREFIX"
] @constant.builtin

(variable_assignment
  name: (word) @clean @constant.builtin
        (#match? @clean "^(AR|AS|CC|CXX|CPP|FC|M2C|PC|CO|GET|LEX|YACC|LINT|MAKEINFO|TEX|TEXI2DVI|WEAVE|CWEAVE|TANGLE|CTANGLE|RM|ARFLAGS|ASFLAGS|CFLAGS|CXXFLAGS|COFLAGS|CPPFLAGS|FFLAGS|GFLAGS|LDFLAGS|LDLIBS|LFLAGS|YFLAGS|PFLAGS|RFLAGS|LINTFLAGS|PRE_INSTALL|POST_INSTALL|NORMAL_INSTALL|PRE_UNINSTALL|POST_UNINSTALL|NORMAL_UNINSTALL|MAKEFILE_LIST|MAKE_RESTARTS|MAKE_TERMOUT|MAKE_TERMERR|\.DEFAULT_GOAL|\.RECIPEPREFIX|\.EXTRA_PREREQS)$"))

(variable_reference
  (word) @clean @constant.builtin
  (#match? @clean "^(AR|AS|CC|CXX|CPP|FC|M2C|PC|CO|GET|LEX|YACC|LINT|MAKEINFO|TEX|TEXI2DVI|WEAVE|CWEAVE|TANGLE|CTANGLE|RM|ARFLAGS|ASFLAGS|CFLAGS|CXXFLAGS|COFLAGS|CPPFLAGS|FFLAGS|GFLAGS|LDFLAGS|LDLIBS|LFLAGS|YFLAGS|PFLAGS|RFLAGS|LINTFLAGS|PRE_INSTALL|POST_INSTALL|NORMAL_INSTALL|PRE_UNINSTALL|POST_UNINSTALL|NORMAL_UNINSTALL|MAKEFILE_LIST|MAKE_RESTARTS|MAKE_TERMOUT|MAKE_TERMERR|\.DEFAULT_GOAL|\.RECIPEPREFIX|\.EXTRA_PREREQS\.VARIABLES|\.FEATURES|\.INCLUDE_DIRS|\.LOADED)$"))

;; Standart targets
(targets
  (word) @constant.macro
  (#match? @constant.macro "^(all|install|install-html|install-dvi|install-pdf|install-ps|uninstall|install-strip|clean|distclean|mostlyclean|maintainer-clean|TAGS|info|dvi|html|pdf|ps|dist|check|installcheck|installdirs)$"))

(targets
  (word) @constant.macro
  (#match? @constant.macro "^(all|install|install-html|install-dvi|install-pdf|install-ps|uninstall|install-strip|clean|distclean|mostlyclean|maintainer-clean|TAGS|info|dvi|html|pdf|ps|dist|check|installcheck|installdirs)$"))

;; Builtin targets
(targets
  (word) @constant.macro
  (#match? @constant.macro "^\.(PHONY|SUFFIXES|DEFAULT|PRECIOUS|INTERMEDIATE|SECONDARY|SECONDEXPANSION|DELETE_ON_ERROR|IGNORE|LOW_RESOLUTION_TIME|SILENT|EXPORT_ALL_VARIABLES|NOTPARALLEL|ONESHELL|POSIX)$"))
)TSQ";

// tree-sitter-scss 1.0.0's own queries/highlights.scm (flake.nix's pinned
// grammar), with its two `(match? ...)` predicates given the `#` upstream
// left off. The query vendored before named nodes this version lacks
// (js_comment, mixin/function statement fields), so ts_query_new
// rejected it and .scss had no highlighting.
static const char *kHighlightsScss = R"TSQ(
(comment) @comment

(tag_name) @tag
(nesting_selector) @tag
(universal_selector) @tag

"~" @operator
">" @operator
"+" @operator
"-" @operator
"*" @operator
"/" @operator
"=" @operator
"^=" @operator
"|=" @operator
"~=" @operator
"$=" @operator
"*=" @operator

"and" @operator
"or" @operator
"not" @operator
"only" @operator

(attribute_selector (plain_value) @string)
(pseudo_element_selector (tag_name) @attribute)
(pseudo_class_selector (class_name) @attribute)

(class_name) @property
(id_name) @property
(namespace_name) @property
(property_name) @property
(feature_name) @property

(attribute_name) @attribute

(function_name) @function

((property_name) @variable
 (#match? @variable "^--"))
((plain_value) @variable
 (#match? @variable "^--"))

"@media" @keyword
"@import" @keyword
"@charset" @keyword
"@namespace" @keyword
"@supports" @keyword
"@keyframes" @keyword
(at_keyword) @keyword
(to) @keyword
(from) @keyword
(important) @keyword

(string_value) @string
(color_value) @string.special

(integer_value) @number
(float_value) @number
(unit) @type

"#" @punctuation.delimiter
"," @punctuation.delimiter
)TSQ";

// tree-sitter-svelte 0.11.0's own queries/highlights.scm: the nvim query
// vendored before used keyword tokens ("as", "key", "snippet", ...) this
// version doesn't have, so the whole query failed to compile.
static const char *kHighlightsSvelte = R"TSQ(
; Special identifiers
;--------------------

; TODO:
((element (start_tag (tag_name) @_tag) (text) @text.title)
 (#match? @_tag "^(h[0-9]|title)$"))

((element (start_tag (tag_name) @_tag) (text) @text.strong)
 (#match? @_tag "^(strong|b)$"))

((element (start_tag (tag_name) @_tag) (text) @text.emphasis)
 (#match? @_tag "^(em|i)$"))

((element (start_tag (tag_name) @_tag) (text) @text.strike)
 (#match? @_tag "^(s|del)$"))

((element (start_tag (tag_name) @_tag) (text) @text.underline)
 (#eq? @_tag "u"))

((element (start_tag (tag_name) @_tag) (text) @text.literal)
 (#match? @_tag "^(code|kbd)$"))

((element (start_tag (tag_name) @_tag) (text) @text.uri)
 (#eq? @_tag "a"))

((attribute
   (attribute_name) @_attr
   (quoted_attribute_value (attribute_value) @text.uri))
 (#match? @_attr "^(href|src)$"))

(tag_name) @tag
(attribute_name) @property
(erroneous_end_tag_name) @error
(comment) @comment

[
  (attribute_value)
  (quoted_attribute_value)
] @string

[
  (text)
  (raw_text_expr)
] @none

[
  (special_block_keyword)
  (then)
  (as)
] @keyword

[
  "{"
  "}"
] @punctuation.bracket

"=" @operator

[
  "<"
  ">"
  "</"
  "/>"
  "#"
  ":"
  "/"
  "@"
] @tag.delimiter
)TSQ";

static const char *kHighlightsToml = R"TSQ(
; Properties
;-----------

(bare_key) @type

(quoted_key) @string

(pair
  (bare_key)) @property

(pair
  (dotted_key
    (bare_key) @property))

; Literals
;---------

(boolean) @boolean

(comment) @comment

(string) @string

[
  (integer)
  (float)
] @number

[
  (offset_date_time)
  (local_date_time)
  (local_date)
  (local_time)
] @string.special

; Punctuation
;------------

[
  "."
  ","
] @punctuation.delimiter

"=" @operator

[
  "["
  "]"
  "[["
  "]]"
  "{"
  "}"
] @punctuation.bracket
)TSQ";

static const char *kHighlightsVim = R"TSQ(
(identifier) @variable

((identifier) @constant
  (#match? @constant "^[A-Z][A-Z_0-9]*$"))

; Keywords
[
  "if"
  "else"
  "elseif"
  "endif"
] @keyword.conditional

[
  "try"
  "catch"
  "finally"
  "endtry"
  "throw"
] @keyword.exception

[
  "for"
  "endfor"
  "in"
  "while"
  "endwhile"
  "break"
  "continue"
] @keyword.repeat

[
  "function"
  "endfunction"
] @keyword.function

; Function related
(function_declaration
  name: (_) @function)

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (scoped_identifier
    (identifier) @function.call))

(parameters
  (identifier) @variable.parameter)

(default_parameter
  (identifier) @variable.parameter)

[
  (bang)
  (spread)
] @punctuation.special

[
  (no_option)
  (inv_option)
  (default_option)
  (option_name)
] @variable.builtin

[
  (scope)
  "a:"
  "$"
] @module

; Commands and user defined commands
[
  "let"
  "unlet"
  "const"
  "call"
  "execute"
  "normal"
  "set"
  "setfiletype"
  "setlocal"
  "silent"
  "echo"
  "echon"
  "echohl"
  "echomsg"
  "echoerr"
  "autocmd"
  "augroup"
  "return"
  "syntax"
  "filetype"
  "source"
  "lua"
  "ruby"
  "perl"
  "python"
  "highlight"
  "command"
  "delcommand"
  "comclear"
  "colorscheme"
  "scriptencoding"
  "startinsert"
  "stopinsert"
  "global"
  "runtime"
  "wincmd"
  "cnext"
  "cprevious"
  "cNext"
  "tab"
  "vertical"
  "leftabove"
  "aboveleft"
  "rightbelow"
  "belowright"
  "topleft"
  "botright"
  (unknown_command_name)
  "edit"
  "enew"
  "find"
  "ex"
  "visual"
  "view"
  "eval"
  "sign"
  "substitute"
] @keyword

(map_statement
  cmd: _ @keyword)

(keycode) @character.special

(command_name) @function.macro

; Filetype command
(filetype_statement
  [
    "detect"
    "plugin"
    "indent"
    "on"
    "off"
  ] @keyword)

; Syntax command
(syntax_statement
  (keyword) @string)

(syntax_statement
  [
    "enable"
    "on"
    "off"
    "reset"
    "case"
    "spell"
    "foldlevel"
    "iskeyword"
    "keyword"
    "match"
    "cluster"
    "region"
    "clear"
    "include"
  ] @keyword)

(syntax_argument
  name: _ @keyword)

[
  "<buffer>"
  "<nowait>"
  "<silent>"
  "<script>"
  "<expr>"
  "<unique>"
] @constant.builtin

(augroup_name) @module

(au_event) @constant

(normal_statement
  (commands) @constant)

; Highlight command
(hl_attribute
  key: _ @property
  val: _ @constant)

(hl_group) @type

(highlight_statement
  [
    "default"
    "link"
    "clear"
  ] @keyword)

; Command command
(command) @string

(command_attribute
  name: _ @property)

(command_attribute
  val: (behavior
    _ @constant))

; Edit command
(plus_plus_opt
  val: _? @constant) @property

(plus_cmd
  "+" @property) @property

; Runtime command
(runtime_statement
  (where) @keyword.operator)

; Colorscheme command
(colorscheme_statement
  (name) @string)

; Scriptencoding command
(scriptencoding_statement
  (encoding) @string.special)

; Literals
(string_literal) @string

(integer_literal) @number

(float_literal) @number.float

(comment) @comment

(line_continuation_comment) @comment

(shebang) @keyword.directive

(pattern) @string.special

(pattern_multi) @string.regexp

(filename) @string.special.path

(heredoc
  (body) @string)

(heredoc
  (parameter) @keyword)

(script
  (parameter) @keyword)

[
  (marker_definition)
  (endmarker)
] @label

(literal_dictionary
  (literal_key) @property)

((scoped_identifier
  (scope) @_scope
  .
  (identifier) @boolean)
  (#eq? @_scope "v:")
  (#any-of? @boolean "true" "false"))

; Operators
[
  "||"
  "&&"
  "&"
  "+"
  "-"
  "*"
  "/"
  "%"
  ".."
  "is"
  "isnot"
  "=="
  "!="
  ">"
  ">="
  "<"
  "<="
  "=~"
  "!~"
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  ".="
  "..="
  "<<"
  "=<<"
  (match_case)
] @operator

; Some characters have different meanings based on the context
(unary_operation
  "!" @operator)

(binary_operation
  "." @operator)

(lua_statement
  "=" @keyword)

; Punctuation
[
  "("
  ")"
  "{"
  "}"
  "["
  "]"
  "#{"
] @punctuation.bracket

(field_expression
  "." @punctuation.delimiter)

[
  ","
  ":"
] @punctuation.delimiter

(ternary_expression
  [
    "?"
    ":"
  ] @keyword.conditional.ternary)

; Options
((set_value) @number
  (#match? @number "^[0-9]+([.][0-9]+)?$"))

(inv_option
  "!" @operator)

(set_item
  "?" @operator)

((set_item
  option: (option_name) @_option
  value: (set_value) @function)
  (#any-of? @_option "tagfunc" "tfu" "completefunc" "cfu" "omnifunc" "ofu" "operatorfunc" "opfunc"))
)TSQ";

static const char *kHighlightsVue = R"TSQ(
; inherits: html_tags

[
  "["
  "]"
] @punctuation.bracket

[
  ":"
  "."
] @character.special

[
  (interpolation)
  "@"
] @punctuation.special

(interpolation
  (raw_text) @none)

(dynamic_directive_inner_value) @variable

(directive_name) @tag.attribute

; Accessing a component object's field
(":"
  .
  (directive_value) @variable.member)

("."
  .
  (directive_value) @property)

; @click is like onclick for HTML
("@"
  .
  (directive_value) @function.method)

; Used in v-slot, declaring position the element should be put in
("#"
  .
  (directive_value) @variable)

(directive_attribute
  (quoted_attribute_value) @punctuation.special)

(directive_attribute
  (quoted_attribute_value
    (attribute_value) @none))

(directive_modifier) @function.method

((template_element) @_template
  (#set! @_template bo.commentstring "<!-- %s -->"))
)TSQ";

static const char *kHighlightsXml = R"TSQ(
;; XML declaration

"xml" @keyword

[ "version" "encoding" "standalone" ] @property

(EncName) @string.special

(VersionNum) @number

[ "yes" "no" ] @boolean

;; Processing instructions

(PI) @embedded

(PI (PITarget) @keyword)

;; Element declaration

(elementdecl
  "ELEMENT" @keyword
  (Name) @tag)

(contentspec
  (_ (Name) @property))

"#PCDATA" @type.builtin

[ "EMPTY" "ANY" ] @string.special.symbol

[ "*" "?" "+" ] @operator

;; Entity declaration

(GEDecl
  "ENTITY" @keyword
  (Name) @constant)

(GEDecl (EntityValue) @string)

(NDataDecl
  "NDATA" @keyword
  (Name) @label)

;; Parsed entity declaration

(PEDecl
  "ENTITY" @keyword
  "%" @operator
  (Name) @constant)

(PEDecl (EntityValue) @string)

;; Notation declaration

(NotationDecl
  "NOTATION" @keyword
  (Name) @constant)

(NotationDecl
  (ExternalID
    (SystemLiteral (URI) @string.special)))

;; Attlist declaration

(AttlistDecl
  "ATTLIST" @keyword
  (Name) @tag)

(AttDef (Name) @property)

(AttDef (Enumeration (Nmtoken) @string))

(DefaultDecl (AttValue) @string)

[
  (StringType)
  (TokenizedType)
] @type.builtin

(NotationType "NOTATION" @type.builtin)

[
  "#REQUIRED"
  "#IMPLIED"
  "#FIXED"
] @attribute

;; Entities

(EntityRef) @constant

((EntityRef) @constant.builtin
 (#any-of? @constant.builtin
   "&amp;" "&lt;" "&gt;" "&quot;" "&apos;"))

(CharRef) @constant

(PEReference) @constant

;; External references

[ "PUBLIC" "SYSTEM" ] @keyword

(PubidLiteral) @string.special

(SystemLiteral (URI) @markup.link)

;; Processing instructions

(XmlModelPI "xml-model" @keyword)

(StyleSheetPI "xml-stylesheet" @keyword)

(PseudoAtt (Name) @property)

(PseudoAtt (PseudoAttValue) @string)

;; Doctype declaration

(doctypedecl "DOCTYPE" @keyword)

(doctypedecl (Name) @type)

;; Tags

(STag (Name) @tag)

(ETag (Name) @tag)

(EmptyElemTag (Name) @tag)

;; Attributes

(Attribute (Name) @property)

(Attribute (AttValue) @string)

;; Delimiters & punctuation

[
 "<?" "?>"
 "<!" "]]>"
 "<" ">"
 "</" "/>"
] @punctuation.delimiter

[ "(" ")" "[" "]" ] @punctuation.bracket

[ "\"" "'" ] @punctuation.delimiter

[ "," "|" "=" ] @operator

;; Text

(CharData) @markup

(CDSect
  (CDStart) @markup.heading
  (CData) @markup.raw
  "]]>" @markup.heading)

;; Misc

(Comment) @comment

(ERROR) @error
)TSQ";

static const char *kHighlightsYaml = R"TSQ(
(boolean_scalar) @boolean

(null_scalar) @constant.builtin

[
  (double_quote_scalar)
  (single_quote_scalar)
  (block_scalar)
  (string_scalar)
] @string

[
  (integer_scalar)
  (float_scalar)
] @number

(comment) @comment

[
  (anchor_name)
  (alias_name)
] @label

(tag) @type

[
  (yaml_directive)
  (tag_directive)
  (reserved_directive)
] @attribute

(block_mapping_pair
  key: (flow_node
    [
      (double_quote_scalar)
      (single_quote_scalar)
    ] @property))

(block_mapping_pair
  key: (flow_node
    (plain_scalar
      (string_scalar) @property)))

(flow_mapping
  (_
    key: (flow_node
      [
        (double_quote_scalar)
        (single_quote_scalar)
      ] @property)))

(flow_mapping
  (_
    key: (flow_node
      (plain_scalar
        (string_scalar) @property))))

[
  ","
  "-"
  ":"
  ">"
  "?"
  "|"
] @punctuation.delimiter

[
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  "*"
  "&"
  "---"
  "..."
] @punctuation.special
)TSQ";

static const char *kHighlightsZig = R"TSQ(
; Variables

(identifier) @variable

; Parameters

(parameter
  name: (identifier) @variable.parameter)

; Types

(parameter
  type: (identifier) @type)

((identifier) @type
  (#lua-match? @type "^[A-Z_][a-zA-Z0-9_]*"))

(variable_declaration
  (identifier) @type
  "="
  [
    (struct_declaration)
    (enum_declaration)
    (union_declaration)
    (opaque_declaration)
  ])

[
  (builtin_type)
  "anyframe"
] @type.builtin

; Constants

((identifier) @constant
  (#lua-match? @constant "^[A-Z][A-Z_0-9]+$"))

[
  "null"
  "unreachable"
  "undefined"
] @constant.builtin

(field_expression
  .
  member: (identifier) @constant)

(enum_declaration
  (container_field
    type: (identifier) @constant))

; Labels

(block_label (identifier) @label)

(break_label (identifier) @label)

; Fields

(field_initializer
  .
  (identifier) @variable.member)

(field_expression
  (_)
  member: (identifier) @variable.member)

(container_field
  name: (identifier) @variable.member)

(initializer_list
  (assignment_expression
      left: (field_expression
              .
              member: (identifier) @variable.member)))

; Functions

(builtin_identifier) @function.builtin

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (field_expression
    member: (identifier) @function.call))

(function_declaration
  name: (identifier) @function)

; Modules

(variable_declaration
  (identifier) @module
  (builtin_function
    (builtin_identifier) @keyword.import
    (#any-of? @keyword.import "@import" "@cImport")))

; Builtins

[
  "c"
  "..."
] @variable.builtin

((identifier) @variable.builtin
  (#eq? @variable.builtin "_"))

(calling_convention
  (identifier) @variable.builtin)

; Keywords

[
  "asm"
  "defer"
  "errdefer"
  "test"
  "error"
  "const"
  "var"
] @keyword

[
  "struct"
  "union"
  "enum"
  "opaque"
] @keyword.type

[
  "async"
  "await"
  "suspend"
  "nosuspend"
  "resume"
] @keyword.coroutine

"fn" @keyword.function

[
  "and"
  "or"
  "orelse"
] @keyword.operator

"return" @keyword.return

[
  "if"
  "else"
  "switch"
] @keyword.conditional

[
  "for"
  "while"
  "break"
  "continue"
] @keyword.repeat

[
  "usingnamespace"
  "export"
] @keyword.import

[
  "try"
  "catch"
] @keyword.exception

[
  "volatile"
  "allowzero"
  "noalias"
  "addrspace"
  "align"
  "callconv"
  "linksection"
  "pub"
  "inline"
  "noinline"
  "extern"
  "comptime"
  "packed"
  "threadlocal"
] @keyword.modifier

; Operator

[
  "="
  "*="
  "*%="
  "*|="
  "/="
  "%="
  "+="
  "+%="
  "+|="
  "-="
  "-%="
  "-|="
  "<<="
  "<<|="
  ">>="
  "&="
  "^="
  "|="
  "!"
  "~"
  "-"
  "-%"
  "&"
  "=="
  "!="
  ">"
  ">="
  "<="
  "<"
  "&"
  "^"
  "|"
  "<<"
  ">>"
  "<<|"
  "+"
  "++"
  "+%"
  "-%"
  "+|"
  "-|"
  "*"
  "/"
  "%"
  "**"
  "*%"
  "*|"
  "||"
  ".*"
  ".?"
  "?"
  ".."
] @operator

; Literals

(character) @character

([
  (string)
  (multiline_string)
] @string
  (#set! "priority" 95))

(integer) @number

(float) @number.float

(boolean) @boolean

(escape_sequence) @string.escape

; Punctuation

[
  "["
  "]"
  "("
  ")"
  "{"
  "}"
] @punctuation.bracket

[
  ";"
  "."
  ","
  ":"
  "=>"
  "->"
] @punctuation.delimiter

(payload "|" @punctuation.bracket)

; Comments

(comment) @comment @spell

((comment) @comment.documentation
  (#lua-match? @comment.documentation "^//!"))
)TSQ";

static const char *kHighlightsCmake = R"TSQ(
(normal_command
  (identifier)
  (argument_list
    (argument
      (unquoted_argument)) @constant)
  (#lua-match? @constant "^[%u@][%u%d_]+$"))

[
  (quoted_argument)
  (bracket_argument)
] @string

(variable_ref) @none

(variable) @variable

[
  (bracket_comment)
  (line_comment)
] @comment @spell

(normal_command
  (identifier) @function)

[
  "ENV"
  "CACHE"
] @module

[
  "$"
  "{"
  "}"
] @punctuation.special

[
  "("
  ")"
] @punctuation.bracket

[
  (function)
  (endfunction)
  (macro)
  (endmacro)
] @keyword.function

[
  (if)
  (elseif)
  (else)
  (endif)
] @keyword.conditional

[
  (foreach)
  (endforeach)
  (while)
  (endwhile)
] @keyword.repeat

(normal_command
  (identifier) @keyword.repeat
  (#match? @keyword.repeat "^([cC][oO][nN][tT][iI][nN][uU][eE]|[bB][rR][eE][aA][kK])$"))

(normal_command
  (identifier) @keyword.return
  (#match? @keyword.return "^[rR][eE][tT][uU][rR][nN]$"))

(function_command
  (function)
  (argument_list
    .
    (argument) @function
    (argument)* @variable.parameter))

(macro_command
  (macro)
  (argument_list
    .
    (argument) @function.macro
    (argument)* @variable.parameter))

(block_def
  (block_command
    (block) @function.builtin
    (argument_list
      (argument
        (unquoted_argument) @constant
        (#any-of? @constant "SCOPE_FOR" "POLICIES" "VARIABLES" "PROPAGATE")
      )*
    )?
  )
  (endblock_command (endblock) @function.builtin))

((argument) @boolean
  (#match? @boolean "^(1|[oO][nN]|[yY][eE][sS]|[tT][rR][uU][eE]|[yY]|0|[oO][fF][fF]|[nN][oO]|[fF][aA][lL][sS][eE]|[nN]|[iI][gG][nN][oO][rR][eE]|[nN][oO][tT][fF][oO][uU][nN][dD]|.*-[nN][oO][tT][fF][oO][uU][nN][dD])$"))

(if_command
  (if)
  (argument_list
    (argument) @keyword.operator)
  (#any-of? @keyword.operator
    "NOT" "AND" "OR" "COMMAND" "POLICY" "TARGET" "TEST" "DEFINED" "IN_LIST" "EXISTS" "IS_NEWER_THAN"
    "IS_DIRECTORY" "IS_SYMLINK" "IS_ABSOLUTE" "MATCHES" "LESS" "GREATER" "EQUAL" "LESS_EQUAL"
    "GREATER_EQUAL" "STRLESS" "STRGREATER" "STREQUAL" "STRLESS_EQUAL" "STRGREATER_EQUAL"
    "VERSION_LESS" "VERSION_GREATER" "VERSION_EQUAL" "VERSION_LESS_EQUAL" "VERSION_GREATER_EQUAL"))

(elseif_command
  (elseif)
  (argument_list
    (argument) @keyword.operator)
  (#any-of? @keyword.operator
    "NOT" "AND" "OR" "COMMAND" "POLICY" "TARGET" "TEST" "DEFINED" "IN_LIST" "EXISTS" "IS_NEWER_THAN"
    "IS_DIRECTORY" "IS_SYMLINK" "IS_ABSOLUTE" "MATCHES" "LESS" "GREATER" "EQUAL" "LESS_EQUAL"
    "GREATER_EQUAL" "STRLESS" "STRGREATER" "STREQUAL" "STRLESS_EQUAL" "STRGREATER_EQUAL"
    "VERSION_LESS" "VERSION_GREATER" "VERSION_EQUAL" "VERSION_LESS_EQUAL" "VERSION_GREATER_EQUAL"))

(normal_command
  (identifier) @function.builtin
  (#match? @function.builtin
    "^([cC][mM][aA][kK][eE]_[hH][oO][sS][tT]_[sS][yY][sS][tT][eE][mM]_[iI][nN][fF][oO][rR][mM][aA][tT][iI][oO][nN]|[cC][mM][aA][kK][eE]_[lL][aA][nN][gG][uU][aA][gG][eE]|[cC][mM][aA][kK][eE]_[mM][iI][nN][iI][mM][uU][mM]_[rR][eE][qQ][uU][iI][rR][eE][dD]|[cC][mM][aA][kK][eE]_[pP][aA][rR][sS][eE]_[aA][rR][gG][uU][mM][eE][nN][tT][sS]|[cC][mM][aA][kK][eE]_[pP][aA][tT][hH]|[cC][mM][aA][kK][eE]_[pP][oO][lL][iI][cC][yY]|[cC][oO][nN][fF][iI][gG][uU][rR][eE]_[fF][iI][lL][eE]|[eE][xX][eE][cC][uU][tT][eE]_[pP][rR][oO][cC][eE][sS][sS]|[fF][iI][lL][eE]|[fF][iI][nN][dD]_[fF][iI][lL][eE]|[fF][iI][nN][dD]_[lL][iI][bB][rR][aA][rR][yY]|[fF][iI][nN][dD]_[pP][aA][cC][kK][aA][gG][eE]|[fF][iI][nN][dD]_[pP][aA][tT][hH]|[fF][iI][nN][dD]_[pP][rR][oO][gG][rR][aA][mM]|[fF][oO][rR][eE][aA][cC][hH]|[gG][eE][tT]_[cC][mM][aA][kK][eE]_[pP][rR][oO][pP][eE][rR][tT][yY]|[gG][eE][tT]_[dD][iI][rR][eE][cC][tT][oO][rR][yY]_[pP][rR][oO][pP][eE][rR][tT][yY]|[gG][eE][tT]_[fF][iI][lL][eE][nN][aA][mM][eE]_[cC][oO][mM][pP][oO][nN][eE][nN][tT]|[gG][eE][tT]_[pP][rR][oO][pP][eE][rR][tT][yY]|[iI][nN][cC][lL][uU][dD][eE]|[iI][nN][cC][lL][uU][dD][eE]_[gG][uU][aA][rR][dD]|[lL][iI][sS][tT]|[mM][aA][cC][rR][oO]|[mM][aA][rR][kK]_[aA][sS]_[aA][dD][vV][aA][nN][cC][eE][dD]|[mM][aA][tT][hH]|[mM][eE][sS][sS][aA][gG][eE]|[oO][pP][tT][iI][oO][nN]|[sS][eE][pP][aA][rR][aA][tT][eE]_[aA][rR][gG][uU][mM][eE][nN][tT][sS]|[sS][eE][tT]|[sS][eE][tT]_[dD][iI][rR][eE][cC][tT][oO][rR][yY]_[pP][rR][oO][pP][eE][rR][tT][iI][eE][sS]|[sS][eE][tT]_[pP][rR][oO][pP][eE][rR][tT][yY]|[sS][iI][tT][eE]_[nN][aA][mM][eE]|[sS][tT][rR][iI][nN][gG]|[uU][nN][sS][eE][tT]|[vV][aA][rR][iI][aA][bB][lL][eE]_[wW][aA][tT][cC][hH]|[aA][dD][dD]_[cC][oO][mM][pP][iI][lL][eE]_[dD][eE][fF][iI][nN][iI][tT][iI][oO][nN][sS]|[aA][dD][dD]_[cC][oO][mM][pP][iI][lL][eE]_[oO][pP][tT][iI][oO][nN][sS]|[aA][dD][dD]_[cC][uU][sS][tT][oO][mM]_[cC][oO][mM][mM][aA][nN][dD]|[aA][dD][dD]_[cC][uU][sS][tT][oO][mM]_[tT][aA][rR][gG][eE][tT]|[aA][dD][dD]_[dD][eE][fF][iI][nN][iI][tT][iI][oO][nN][sS]|[aA][dD][dD]_[dD][eE][pP][eE][nN][dD][eE][nN][cC][iI][eE][sS]|[aA][dD][dD]_[eE][xX][eE][cC][uU][tT][aA][bB][lL][eE]|[aA][dD][dD]_[lL][iI][bB][rR][aA][rR][yY]|[aA][dD][dD]_[lL][iI][nN][kK]_[oO][pP][tT][iI][oO][nN][sS]|[aA][dD][dD]_[sS][uU][bB][dD][iI][rR][eE][cC][tT][oO][rR][yY]|[aA][dD][dD]_[tT][eE][sS][tT]|[aA][uU][xX]_[sS][oO][uU][rR][cC][eE]_[dD][iI][rR][eE][cC][tT][oO][rR][yY]|[bB][uU][iI][lL][dD]_[cC][oO][mM][mM][aA][nN][dD]|[cC][rR][eE][aA][tT][eE]_[tT][eE][sS][tT]_[sS][oO][uU][rR][cC][eE][lL][iI][sS][tT]|[dD][eE][fF][iI][nN][eE]_[pP][rR][oO][pP][eE][rR][tT][yY]|[eE][nN][aA][bB][lL][eE]_[lL][aA][nN][gG][uU][aA][gG][eE]|[eE][nN][aA][bB][lL][eE]_[tT][eE][sS][tT][iI][nN][gG]|[eE][xX][pP][oO][rR][tT]|[fF][lL][tT][kK]_[wW][rR][aA][pP]_[uU][iI]|[gG][eE][tT]_[sS][oO][uU][rR][cC][eE]_[fF][iI][lL][eE]_[pP][rR][oO][pP][eE][rR][tT][yY]|[gG][eE][tT]_[tT][aA][rR][gG][eE][tT]_[pP][rR][oO][pP][eE][rR][tT][yY]|[gG][eE][tT]_[tT][eE][sS][tT]_[pP][rR][oO][pP][eE][rR][tT][yY]|[iI][nN][cC][lL][uU][dD][eE]_[dD][iI][rR][eE][cC][tT][oO][rR][iI][eE][sS]|[iI][nN][cC][lL][uU][dD][eE]_[eE][xX][tT][eE][rR][nN][aA][lL]_[mM][sS][pP][rR][oO][jJ][eE][cC][tT]|[iI][nN][cC][lL][uU][dD][eE]_[rR][eE][gG][uU][lL][aA][rR]_[eE][xX][pP][rR][eE][sS][sS][iI][oO][nN]|[iI][nN][sS][tT][aA][lL][lL]|[lL][iI][nN][kK]_[dD][iI][rR][eE][cC][tT][oO][rR][iI][eE][sS]|[lL][iI][nN][kK]_[lL][iI][bB][rR][aA][rR][iI][eE][sS]|[lL][oO][aA][dD]_[cC][aA][cC][hH][eE]|[pP][rR][oO][jJ][eE][cC][tT]|[rR][eE][mM][oO][vV][eE]_[dD][eE][fF][iI][nN][iI][tT][iI][oO][nN][sS]|[sS][eE][tT]_[sS][oO][uU][rR][cC][eE]_[fF][iI][lL][eE][sS]_[pP][rR][oO][pP][eE][rR][tT][iI][eE][sS]|[sS][eE][tT]_[tT][aA][rR][gG][eE][tT]_[pP][rR][oO][pP][eE][rR][tT][iI][eE][sS]|[sS][eE][tT]_[tT][eE][sS][tT][sS]_[pP][rR][oO][pP][eE][rR][tT][iI][eE][sS]|[sS][oO][uU][rR][cC][eE]_[gG][rR][oO][uU][pP]|[tT][aA][rR][gG][eE][tT]_[cC][oO][mM][pP][iI][lL][eE]_[dD][eE][fF][iI][nN][iI][tT][iI][oO][nN][sS]|[tT][aA][rR][gG][eE][tT]_[cC][oO][mM][pP][iI][lL][eE]_[fF][eE][aA][tT][uU][rR][eE][sS]|[tT][aA][rR][gG][eE][tT]_[cC][oO][mM][pP][iI][lL][eE]_[oO][pP][tT][iI][oO][nN][sS]|[tT][aA][rR][gG][eE][tT]_[iI][nN][cC][lL][uU][dD][eE]_[dD][iI][rR][eE][cC][tT][oO][rR][iI][eE][sS]|[tT][aA][rR][gG][eE][tT]_[lL][iI][nN][kK]_[dD][iI][rR][eE][cC][tT][oO][rR][iI][eE][sS]|[tT][aA][rR][gG][eE][tT]_[lL][iI][nN][kK]_[lL][iI][bB][rR][aA][rR][iI][eE][sS]|[tT][aA][rR][gG][eE][tT]_[lL][iI][nN][kK]_[oO][pP][tT][iI][oO][nN][sS]|[tT][aA][rR][gG][eE][tT]_[pP][rR][eE][cC][oO][mM][pP][iI][lL][eE]_[hH][eE][aA][dD][eE][rR][sS]|[tT][aA][rR][gG][eE][tT]_[sS][oO][uU][rR][cC][eE][sS]|[tT][rR][yY]_[cC][oO][mM][pP][iI][lL][eE]|[tT][rR][yY]_[rR][uU][nN]|[cC][tT][eE][sS][tT]_[bB][uU][iI][lL][dD]|[cC][tT][eE][sS][tT]_[cC][oO][nN][fF][iI][gG][uU][rR][eE]|[cC][tT][eE][sS][tT]_[cC][oO][vV][eE][rR][aA][gG][eE]|[cC][tT][eE][sS][tT]_[eE][mM][pP][tT][yY]_[bB][iI][nN][aA][rR][yY]_[dD][iI][rR][eE][cC][tT][oO][rR][yY]|[cC][tT][eE][sS][tT]_[mM][eE][mM][cC][hH][eE][cC][kK]|[cC][tT][eE][sS][tT]_[rR][eE][aA][dD]_[cC][uU][sS][tT][oO][mM]_[fF][iI][lL][eE][sS]|[cC][tT][eE][sS][tT]_[rR][uU][nN]_[sS][cC][rR][iI][pP][tT]|[cC][tT][eE][sS][tT]_[sS][lL][eE][eE][pP]|[cC][tT][eE][sS][tT]_[sS][tT][aA][rR][tT]|[cC][tT][eE][sS][tT]_[sS][uU][bB][mM][iI][tT]|[cC][tT][eE][sS][tT]_[tT][eE][sS][tT]|[cC][tT][eE][sS][tT]_[uU][pP][dD][aA][tT][eE]|[cC][tT][eE][sS][tT]_[uU][pP][lL][oO][aA][dD])$"))

(normal_command
  (identifier) @_function
  (argument_list
    .
    (argument) @variable)
  (#match? @_function "^[sS][eE][tT]$"))

(normal_command
  (identifier) @_function
  (#match? @_function "^[sS][eE][tT]$")
  (argument_list
    .
    (argument)
    ((argument) @_cache @keyword.modifier
      .
      (argument) @_type @type
      (#any-of? @_cache "CACHE")
      (#any-of? @_type "BOOL" "FILEPATH" "PATH" "STRING" "INTERNAL"))))

(normal_command
  (identifier) @_function
  (#match? @_function "^[uU][nN][sS][eE][tT]$")
  (argument_list
    .
    (argument)
    (argument) @keyword.modifier
    (#any-of? @keyword.modifier "CACHE" "PARENT_SCOPE")))

(normal_command
  (identifier) @_function
  (#match? @_function "^[lL][iI][sS][tT]$")
  (argument_list
    .
    (argument) @constant
    (#any-of? @constant "LENGTH" "GET" "JOIN" "SUBLIST" "FIND")
    .
    (argument) @variable
    (argument) @variable .))

(normal_command
  (identifier) @_function
  (#match? @_function "^[lL][iI][sS][tT]$")
  (argument_list
    .
    (argument) @constant
    .
    (argument) @variable
    (#any-of? @constant
      "APPEND" "FILTER" "INSERT" "POP_BACK" "POP_FRONT" "PREPEND" "REMOVE_ITEM" "REMOVE_AT"
      "REMOVE_DUPLICATES" "REVERSE" "SORT")))

(normal_command
  (identifier) @_function
  (#match? @_function "^[lL][iI][sS][tT]$")
  (argument_list
    .
    (argument) @_transform @constant
    .
    (argument) @variable
    .
    (argument) @_action @constant
    (#eq? @_transform "TRANSFORM")
    (#any-of? @_action "APPEND" "PREPEND" "TOUPPER" "TOLOWER" "STRIP" "GENEX_STRIP" "REPLACE")))

(normal_command
  (identifier) @_function
  (#match? @_function "^[lL][iI][sS][tT]$")
  (argument_list
    .
    (argument) @_transform @constant
    .
    (argument) @variable
    .
    (argument) @_action @constant
    .
    (argument)? @_selector @constant
    (#eq? @_transform "TRANSFORM")
    (#any-of? @_action "APPEND" "PREPEND" "TOUPPER" "TOLOWER" "STRIP" "GENEX_STRIP" "REPLACE")
    (#any-of? @_selector "AT" "FOR" "REGEX")))

(normal_command
  (identifier) @_function
  (#match? @_function "^[lL][iI][sS][tT]$")
  (argument_list
    .
    (argument) @_transform @constant
    (argument) @constant
    .
    (argument) @variable
    (#eq? @_transform "TRANSFORM")
    (#eq? @constant "OUTPUT_VARIABLE")))

(escape_sequence) @string.escape

((source_file
  .
  (line_comment) @keyword.directive @nospell)
  (#lua-match? @keyword.directive "^#!/"))
)TSQ";

static const char *kHighlightsClojure = R"TSQ(
;; Literals

(num_lit) @number

[
  (char_lit)
  (str_lit)
] @string

[
 (bool_lit)
 (nil_lit)
] @constant.builtin

(kwd_lit) @constant

;; Comments

(comment) @comment

;; Treat quasiquotation as operators for the purpose of highlighting.

[
 "'"
 "`"
 "~"
 "@"
 "~@"
] @operator

;; mep's additions (the grammar's own query stops at literals): calls,
;; definitions and the core special forms/macros. Later patterns win, so
;; the keyword list overrides the generic call capture.

(sym_lit namespace: (sym_ns) @namespace)

(list_lit . (sym_lit name: (sym_name) @function.call))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @function)
  (#any-of? @_def "defn" "defn-" "defmacro" "defmulti" "defmethod"))

(list_lit
  . (sym_lit name: (sym_name) @_def)
  . (sym_lit name: (sym_name) @type)
  (#any-of? @_def "defrecord" "deftype" "defprotocol" "definterface" "ns"))

(list_lit
  . (sym_lit name: (sym_name) @keyword)
  (#any-of? @keyword
    "def" "defn" "defn-" "defmacro" "defmulti" "defmethod" "defrecord" "deftype"
    "defprotocol" "definterface" "defonce" "ns" "fn" "let" "letfn" "loop" "recur"
    "if" "if-let" "if-not" "when" "when-let" "when-not" "cond" "condp" "case"
    "do" "doseq" "dotimes" "for" "try" "catch" "finally" "throw" "quote" "var"
    "->" "->>" "as->" "some->" "some->>" "doto" "binding" "and" "or" "not"))

(list_lit
  . (sym_lit name: (sym_name) @function.builtin)
  (#any-of? @function.builtin
    "println" "print" "prn" "str" "format" "map" "filter" "reduce" "apply"
    "first" "rest" "count" "conj" "assoc" "get" "into" "group-by" "range"))

(vec_lit (sym_lit name: (sym_name) @variable.parameter))
)TSQ";

static const char *kHighlightsDart = R"TSQ(
; Variable
(identifier) @variable

; Keywords
; --------------------
[
    (assert_builtin)
    (break_builtin)
    (const_builtin)
    (part_of_builtin)
    (rethrow_builtin)
    (void_type)
    "abstract"
    "as"
    "async"
    "async*"
    "await"
    "base"
    "case"
    "catch"
    "class"
    "continue"
    "covariant"
    "default"
    "deferred"
    "do"
    "else"
    "enum"
    "export"
    "extends"
    "extension"
    "external"
    "factory"
    "final"
    "finally"
    "for"
    "Function"
    "hide"
    "if"
    "implements"
    "import"
    "in"
    "interface"
    "is"
    "late"
    "library"
    "mixin"
    "new"
    "on"
    "part"
    "required"
    "return"
    "sealed"
    "show"
    "static"
    "super"
    "switch"
    "sync*"
    "throw"
    "try"
    "typedef"
    "var"
    "when"
    "while"
    "with"
    "yield"
] @keyword

; Methods
; --------------------

; NOTE: This query is a bit of a work around for the fact that the dart grammar doesn't
; specifically identify a node as a function call
(((identifier) @function (#match? @function "^_?[a-z]"))
 . (selector . (argument_part))) @function

; Operators and Tokens
; --------------------
(template_substitution
  "$" @punctuation.special
  "{" @punctuation.special
  "}" @punctuation.special
) @none

(template_substitution
  "$" @punctuation.special
  (identifier_dollar_escaped) @variable
) @none

(escape_sequence) @string.escape

[
 "@"
 "=>"
 ".."
 "??"
 "=="
 "?"
 ":"
 "&&"
 "%"
 "<"
 ">"
 "="
 ">="
 "<="
 "||"
 "~/"
 (increment_operator)
 (is_operator)
 (prefix_operator)
 (equality_operator)
 (additive_operator)
] @operator

(type_arguments
  "<" @punctuation.bracket
  ">" @punctuation.bracket)

(type_parameters
  "<" @punctuation.bracket
  ">" @punctuation.bracket)

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
]  @punctuation.bracket

; Delimiters
; --------------------
[
  ";"
  "."
  ","
] @punctuation.delimiter

; Types
; --------------------
(type_identifier) @type
((type_identifier) @type.builtin
  (#match? @type.builtin "^(int|double|String|bool|List|Set|Map|Runes|Symbol)$"))
(class_definition
  name: (identifier) @type)
(constructor_signature
  name: (identifier) @type)
(scoped_identifier
  scope: (identifier) @type)
(function_signature
  name: (identifier) @function)
(getter_signature
  "get" @keyword
  (identifier) @function)
(setter_signature
  "set" @keyword
  name: (identifier) @function)
(operator_signature
  "operator" @keyword)

((scoped_identifier
  scope: (identifier) @type
  name: (identifier) @type)
 (#match? @type "^[a-zA-Z]"))

; Enums
; -------------------
(enum_declaration
  name: (identifier) @type)
(enum_constant
  name: (identifier) @identifier.constant)

; Variables
; --------------------
; var keyword
(inferred_type) @keyword

((identifier) @type
 (#match? @type "^_?[A-Z].*[a-z]"))

("Function" @type)

(this) @variable.builtin

; properties

(unconditional_assignable_selector
  (identifier) @property)

(conditional_assignable_selector
  (identifier) @property)

(cascade_section
  (cascade_selector
    (identifier) @property))

((selector
  (unconditional_assignable_selector (identifier) @function))
  (selector (argument_part (arguments)))
)

(cascade_section
  (cascade_selector (identifier) @function)
  (argument_part (arguments))
)

; assignments
(assignment_expression
  left: (assignable_expression) @variable)

(this) @variable.builtin

; Parameters
; --------------------
(formal_parameter
    name: (identifier) @identifier.parameter)

(named_argument
  (label (identifier) @identifier.parameter))

; Literals
; --------------------
[
    (hex_integer_literal)
    (decimal_integer_literal)
    (decimal_floating_point_literal)
    ; TODO: inaccessbile nodes
    ; (octal_integer_literal)
    ; (hex_floating_point_literal)
] @number

(string_literal) @string
(symbol_literal (identifier) @constant) @constant
(true) @boolean
(false) @boolean
(null_literal) @constant.null

(documentation_comment) @comment
(comment) @comment

; Annotations
; --------------------
(annotation
  "@" @attribute
  name: (identifier) @attribute)

; Modern Dart 3+ Features
; --------------------

; Extension Types
(extension_type_declaration
  "type" @keyword)

(extension_type_declaration
  name: (identifier) @type)

(representation_declaration
  name: (identifier) @property)

; Switch Guards ("when")
(switch_expression_case
  "when" @keyword)

(switch_statement_case
  "when" @keyword)

; Patterns & Pattern Matching
(object_pattern
  (identifier) @property)

(record_pattern
  (identifier) @property)

; Record Types
(record_type_field
  (identifier) @variable)
)TSQ";

static const char *kHighlightsDockerfile = R"TSQ(
[
	"FROM"
	"AS"
	"RUN"
	"CMD"
	"LABEL"
	"EXPOSE"
	"ENV"
	"ADD"
	"COPY"
	"ENTRYPOINT"
	"VOLUME"
	"USER"
	"WORKDIR"
	"ARG"
	"ONBUILD"
	"STOPSIGNAL"
	"HEALTHCHECK"
	"SHELL"
	"MAINTAINER"
	"CROSS_BUILD"
	(heredoc_marker)
	(heredoc_end)
] @keyword

[
	":"
	"@"
] @operator

(comment) @comment


(image_spec
	(image_tag
		":" @punctuation.special)
	(image_digest
		"@" @punctuation.special))

[
	(double_quoted_string)
	(single_quoted_string)
	(json_string)
	(heredoc_line)
] @string

(expansion
  [
	"$"
	"{"
	"}"
  ] @punctuation.special
) @none

((variable) @constant
 (#match? @constant "^[A-Z][A-Z_0-9]*$"))
)TSQ";

static const char *kHighlightsProto = R"TSQ(
[
  "syntax"
  "package"
  "option"
  "import"
  "service"
  "rpc"
  "returns"
  "message"
  "enum"
  "oneof"
  "repeated"
  "reserved"
  "to"
] @keyword

[
  (key_type)
  (type)
  (message_name)
  (enum_name)
  (service_name)
  (rpc_name)
]@type

(string) @string

[
  (int_lit)
  (float_lit)
] @number

[
  (true)
  (false)
] @constant.builtin

(comment) @comment

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
]  @punctuation.bracket
)TSQ";

static const char *kHighlightsIni = R"TSQ(
(section_name
  (text) @type) ; consistency with toml

(comment) @comment @spell

[
  "["
  "]"
] @punctuation.bracket

"=" @operator

(setting
  (setting_name) @property)

; (setting_value) @none ; grammar does not support subtypes
)TSQ";

static const char *kHighlightsFish = R"TSQ(
[(double_quote_string) (single_quote_string)] @string
(escape_sequence) @string.escape

(comment) @comment

[(integer) (float)] @number

[
  "&&"
  "||"
  "|"
  "&|"
  "2>|"
  "&"
  ".."
  (direction)
  (stream_redirect)
] @operator

; match operators of test command
(command
  name: (word) @function (#match? @function "^test$")
  argument: (word) @operator (#match? @operator "^(!?=|-[a-zA-Z]+)$"))

; match operators of [ command
(command
  name: (word) @punctuation.bracket (#match? @punctuation.bracket "^\\[$")
  argument: (word) @operator (#match? @operator "^(!?=|-[a-zA-Z]+)$"))

(variable_expansion) @constant

[
 "["
 "]"
 "{"
 "}"
 "("
 ")"
] @punctuation.bracket

"," @punctuation.delimiter

(function_definition name: [(word) (concatenation)] @function)
(command name: (word) @function)

[
 "switch"
 "case"
 "in"
 "begin"
 "function"
 "if"
 "else"
 "end"
 "while"
 "for"
 "not"
 "!"
 "and"
 "or"
 "return"
 (break)
 (continue)
] @keyword
)TSQ";

static const char *kHighlightsGroovy = R"TSQ(
[
  "!in"
  "!instanceof"
  "as"
  "assert"
  "case"
  "catch"
  "class"
  "def"
  "default"
  "else"
  "extends"
  "finally"
  "for"
  "if"
  "import"
  "in"
  "instanceof"
  "package"
  "pipeline"
  "return"
  "switch"
  "try"
  "while"
  (break)
  (continue)
] @keyword

[
  "true"
  "false"
] @boolean

(null) @constant
"this" @variable.builtin

[ 
  "int"
  "char"
  "short"
  "long"
  "boolean"
  "float"
  "double"
  "void"
] @type.builtin

[ 
  "final"
  "private"
  "protected"
  "public"
  "static"
  "synchronized"
] @type.qualifier

(comment) @comment
(shebang) @comment

(string) @string
(string (escape_sequence) @operator)
(string (interpolation ([ "$" ]) @operator))

("(") @punctuation.bracket
(")") @punctuation.bracket
("[") @punctuation.bracket
("]") @punctuation.bracket
("{") @punctuation.bracket
("}") @punctuation.bracket
(":") @punctuation.delimiter
(",") @punctuation.delimiter
(".") @punctuation.delimiter

(number_literal) @number
(identifier) @variable
((identifier) @variable.parameter
  (#is? @variable.parameter "local.parameter"))

((identifier) @constant
  (#match? @constant "^[A-Z][A-Z_]+"))

[ 
  "%" "*" "/" "+" "-" "<<" ">>" ">>>" ".." "..<" "<..<" "<.." "<"
  "<=" ">" ">=" "==" "!=" "<=>" "===" "!==" "=~" "==~" "&" "^" "|"
  "&&" "||" "?:" "+" "*" ".&" ".@" "?." "*." "*" "*:" "++" "--" "!"
] @operator

(string ("/") @string)

(ternary_op ([ "?" ":" ]) @operator)

(map (map_item key: (identifier) @variable.parameter))

(parameter type: (identifier) @type name: (identifier) @variable.parameter)
(generic_param name: (identifier) @variable.parameter)

(declaration type: (identifier) @type)
(function_definition type: (identifier) @type)
(function_declaration type: (identifier) @type)
(class_definition name: (identifier) @type)
(class_definition superclass: (identifier) @type)
(generic_param superclass: (identifier) @type)

(type_with_generics (identifier) @type)
(type_with_generics (generics (identifier) @type))
(generics [ "<" ">" ] @punctuation.bracket)
(generic_parameters [ "<" ">" ] @punctuation.bracket)
; TODO: Class literals with PascalCase

(declaration ("=") @operator)
(assignment ("=") @operator)


(function_call 
  function: (identifier) @function)
(function_call
  function: (dotted_identifier
	  (identifier) @function . ))
(function_call (argument_list
		 (map_item key: (identifier) @variable.parameter)))
(juxt_function_call 
  function: (identifier) @function)
(juxt_function_call
  function: (dotted_identifier
	  (identifier) @function . ))
(juxt_function_call (argument_list 
		      (map_item key: (identifier) @variable.parameter)))

(function_definition 
  function: (identifier) @function)
(function_declaration 
  function: (identifier) @function)

(annotation) @function.macro
(annotation (identifier) @function.macro)
"@interface" @function.macro

"pipeline" @keyword

(groovy_doc) @comment.documentation
(groovy_doc 
  [
    (groovy_doc_param)
    (groovy_doc_throws)
    (groovy_doc_tag)
  ] @string.special)
(groovy_doc (groovy_doc_param (identifier) @variable.parameter))
(groovy_doc (groovy_doc_throws (identifier) @type))
)TSQ";

static const char *kHighlightsElm = R"TSQ(
; Keywords
[
    "if"
    "then"
    "else"
    "let"
    "in"
 ] @keyword.control.elm
(case) @keyword.control.elm
(of) @keyword.control.elm

(colon) @keyword.other.elm
(backslash) @keyword.other.elm
(as) @keyword.other.elm
(port) @keyword.other.elm
(exposing) @keyword.other.elm
(alias) @keyword.other.elm
(infix) @keyword.other.elm

(arrow) @keyword.operator.arrow.elm

(port) @keyword.other.port.elm

(type_annotation(lower_case_identifier) @function.elm)
(port_annotation(lower_case_identifier) @function.elm)
(function_declaration_left(lower_case_identifier) @function.elm)
(function_call_expr target: (value_expr) @function.elm)

(field_access_expr(value_expr(value_qid)) @local.function.elm)
(lower_pattern) @local.function.elm
(record_base_identifier) @local.function.elm


(operator_identifier) @keyword.operator.elm
(eq) @keyword.operator.assignment.elm


"(" @punctuation.section.braces
")" @punctuation.section.braces

"|" @keyword.other.elm
"," @punctuation.separator.comma.elm

(import) @meta.import.elm
(module) @keyword.other.elm

(number_constant_expr) @constant.numeric.elm


(type) @keyword.type.elm

(type_declaration(upper_case_identifier) @storage.type.elm)
(type_ref) @storage.type.elm
(type_alias_declaration name: (upper_case_identifier) @storage.type.elm)

(union_variant(upper_case_identifier) @union.elm)
(union_pattern) @union.elm
(value_expr(upper_case_qid(upper_case_identifier)) @union.elm)

; comments
(line_comment) @comment.elm
(block_comment) @comment.elm

; strings
(string_escape) @character.escape.elm

(open_quote) @string.elm
(close_quote) @string.elm
(regular_string_part) @string.elm

(open_char) @char.elm
(close_char) @char.elm


; glsl
(glsl_content) @source.glsl
)TSQ";

static const char *kHighlightsErlang = R"TSQ(
;; Copyright (c) Facebook, Inc. and its affiliates.
;;
;; Licensed under the Apache License, Version 2.0 (the "License");
;; you may not use this file except in compliance with the License.
;; You may obtain a copy of the License at
;;
;;     http://www.apache.org/licenses/LICENSE-2.0
;;
;; Unless required by applicable law or agreed to in writing, software
;; distributed under the License is distributed on an "AS IS" BASIS,
;; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
;; See the License for the specific language governing permissions and
;; limitations under the License.
;; ---------------------------------------------------------------------

;; Based initially on the contents of https://github.com/WhatsApp/tree-sitter-erlang/issues/2 by @Wilfred
;; and https://github.com/the-mikedavis/tree-sitter-erlang/blob/main/queries/highlights.scm
;;
;; The tests are also based on those in
;; https://github.com/the-mikedavis/tree-sitter-erlang/tree/main/test/highlight
;;


;; First match wins in this file

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Attributes

;; module attribute
(module_attribute
  name: (atom) @module)

;; behaviour
(behaviour_attribute name: (atom) @module)

;; export

;; Import attribute
(import_attribute
    module: (atom) @module)

;; export_type

;; optional_callbacks

;; compile
(compile_options_attribute
    options: (tuple
      expr: (atom)
      expr: (list
        exprs: (binary_op_expr
          lhs: (atom)
          rhs: (integer)))))

;; file attribute

;; record
(record_decl name: (atom) @type)
(record_decl name: (macro_call_expr name: (var) @constant))
(record_field name: (atom) @property)

;; type alias

;; opaque

;; Spec attribute
(spec fun: (atom) @function)
(spec
  module: (module name: (atom) @module)
  fun: (atom) @function)

;; callback
(callback fun: (atom) @function)

;; wild attribute
(wild_attribute name: (attr_name name: (atom) @keyword))

;; fun decl

;; include/include_lib

;; ifdef/ifndef
(pp_ifdef name: (_) @keyword.directive)
(pp_ifndef name: (_) @keyword.directive)

;; define
(pp_define
    lhs: (macro_lhs
      name: (_) @keyword.directive
      args: (var_args args: (var))))
(pp_define
    lhs: (macro_lhs
      name: (var) @constant))


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Functions
(fa fun: (atom) @function)
(type_name name: (atom) @function)
(call expr: (atom) @function)
(function_clause name: (atom) @function)
(internal_fun fun: (atom) @function)

;; This is a fudge, we should check that the operator is '/'
;; But our grammar does not (currently) provide it
(binary_op_expr lhs: (atom) @function rhs: (integer))

;; Others
(remote_module module: (atom) @module)
(remote fun: (atom) @function)
(macro_call_expr name: (var) @keyword.directive args: (_) )
(macro_call_expr name: (var) @constant)
(macro_call_expr name: (atom) @keyword.directive)
(record_field_name name: (atom) @property)
(record_name name: (atom) @type)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; Reserved words
[ "after"
  "and"
  "band"
  "begin"
  "behavior"
  "behaviour"
  "bnot"
  "bor"
  "bsl"
  "bsr"
  "bxor"
  "callback"
  "case"
  "catch"
  "compile"
  "define"
  "div"
  "elif"
  "else"
  "end"
  "endif"
  "export"
  "export_type"
  "file"
  "fun"
  "if"
  "ifdef"
  "ifndef"
  "import"
  "include"
  "include_lib"
  "module"
  "of"
  "opaque"
  "optional_callbacks"
  "or"
  "receive"
  "record"
  "spec"
  "try"
  "type"
  "undef"
  "unit"
  "when"
  "xor"] @keyword

["andalso" "orelse"] @keyword.operator

;; Punctuation
["," "." ";"] @punctuation.delimiter
["(" ")" "{" "}" "[" "]" "<<" ">>"] @punctuation.bracket

;; Operators
["!"
 "->"
 "<-"
 "#"
 "::"
 "|"
 ":"
 "="
 "||"

 "+"
 "-"
 "bnot"
 "not"

 "/"
 "*"
 "div"
 "rem"
 "band"
 "and"

 "+"
 "-"
 "bor"
 "bxor"
 "bsl"
 "bsr"
 "or"
 "xor"

 "++"
 "--"

 "=="
 "/="
 "=<"
 "<"
 ">="
 ">"
 "=:="
 "=/="
 ] @operator

;;; Comments
((var) @comment.discard
 (#match? @comment.discard "^_"))

(dotdotdot) @comment.discard
(comment) @comment

;; Primitive types
(string) @string
(char) @constant
(integer) @number
(var) @variable
(atom) @string.special.symbol
)TSQ";

static const char *kHighlightsElixir = R"TSQ(
; Punctuation

[
 "%"
] @punctuation

[
 ","
 ";"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  "<<"
  ">>"
] @punctuation.bracket

; Literals

[
  (boolean)
  (nil)
] @constant

[
  (integer)
  (float)
] @number

(char) @constant

; Identifiers

; * regular
(identifier) @variable

; * unused
(
  (identifier) @comment.unused
  (#match? @comment.unused "^_")
)

; * special
(
  (identifier) @constant.builtin
  (#any-of? @constant.builtin "__MODULE__" "__DIR__" "__ENV__" "__CALLER__" "__STACKTRACE__")
)

; Comment

(comment) @comment

; Quoted content

(interpolation "#{" @punctuation.special "}" @punctuation.special) @embedded

(escape_sequence) @string.escape

[
  (string)
  (charlist)
] @string

[
  (atom)
  (quoted_atom)
  (keyword)
  (quoted_keyword)
] @string.special.symbol

; Note that we explicitly target sigil quoted start/end, so they are not overridden by delimiters

(sigil
  (sigil_name) @__name__
  quoted_start: _ @string.special
  quoted_end: _ @string.special) @string.special

(sigil
  (sigil_name) @__name__
  quoted_start: _ @string
  quoted_end: _ @string
  (#match? @__name__ "^[sS]$")) @string

(sigil
  (sigil_name) @__name__
  quoted_start: _ @string.regex
  quoted_end: _ @string.regex
  (#match? @__name__ "^[rR]$")) @string.regex

; Calls

; * local function call
(call
  target: (identifier) @function)

; * remote function call
(call
  target: (dot
    right: (identifier) @function))

; * field without parentheses or block
(call
  target: (dot
    right: (identifier) @property)
  .)

; * remote call without parentheses or block (overrides above)
(call
  target: (dot
    left: [
      (alias)
      (atom)
    ]
    right: (identifier) @function)
  .)

; * definition keyword
(call
  target: (identifier) @keyword
  (#any-of? @keyword "def" "defdelegate" "defexception" "defguard" "defguardp" "defimpl" "defmacro" "defmacrop" "defmodule" "defn" "defnp" "defoverridable" "defp" "defprotocol" "defstruct"))

; * kernel or special forms keyword
(call
  target: (identifier) @keyword
  (#any-of? @keyword "alias" "case" "cond" "for" "if" "import" "quote" "raise" "receive" "require" "reraise" "super" "throw" "try" "unless" "unquote" "unquote_splicing" "use" "with"))

; * just identifier in function definition
(call
  target: (identifier) @keyword
  (arguments
    [
      (identifier) @function
      (binary_operator
        left: (identifier) @function
        operator: "when")
    ])
  (#any-of? @keyword "def" "defdelegate" "defguard" "defguardp" "defmacro" "defmacrop" "defn" "defnp" "defp"))

; * pipe into identifier (function call)
(binary_operator
  operator: "|>"
  right: (identifier) @function)

; * pipe into identifier (definition)
(call
  target: (identifier) @keyword
  (arguments
    (binary_operator
      operator: "|>"
      right: (identifier) @variable))
  (#any-of? @keyword "def" "defdelegate" "defguard" "defguardp" "defmacro" "defmacrop" "defn" "defnp" "defp"))

; * pipe into field without parentheses (function call)
(binary_operator
  operator: "|>"
  right: (call
    target: (dot
      right: (identifier) @function)))

; Operators

; * capture operand
(unary_operator
  operator: "&"
  operand: (integer) @operator)

(operator_identifier) @operator

(unary_operator
  operator: _ @operator)

(binary_operator
  operator: _ @operator)

(dot
  operator: _ @operator)

(stab_clause
  operator: _ @operator)

; * module attribute
(unary_operator
  operator: "@" @attribute
  operand: [
    (identifier) @attribute
    (call
      target: (identifier) @attribute)
    (boolean) @attribute
    (nil) @attribute
  ])

; * doc string
(unary_operator
  operator: "@" @comment.doc
  operand: (call
    target: (identifier) @comment.doc.__attribute__
    (arguments
      [
        (string) @comment.doc
        (charlist) @comment.doc
        (sigil
          quoted_start: _ @comment.doc
          quoted_end: _ @comment.doc) @comment.doc
        (boolean) @comment.doc
      ]))
  (#any-of? @comment.doc.__attribute__ "moduledoc" "typedoc" "doc"))

; Module

(alias) @module

(call
  target: (dot
    left: (atom) @module))

; Reserved keywords

["when" "and" "or" "not" "in" "not in" "fn" "do" "end" "catch" "rescue" "after" "else"] @keyword
)TSQ";

static const char *kHighlightsOrg = R"TSQ(
; A Note on anonymous nodes (represented in a query file as strings). As of
; right now, anonymous nodes can not be anchored.
; See https://github.com/tree-sitter/tree-sitter/issues/1461

; Example highlighting for headlines. The headlines here will be matched
; cyclically, easily extended to match however your heart desires.
(headline (stars) @OrgStars1 (#match? @OrgStars1 "^(\\*{3})*\\*$") (item) @OrgHeadlineLevel1)
(headline (stars) @OrgStars2 (#match? @OrgStars2 "^(\\*{3})*\\*\\*$") (item) @OrgHeadlineLevel2)
(headline (stars) @OrgStars3 (#match? @OrgStars3 "^(\\*{3})*\\*\\*\\*$") (item) @OrgHeadlineLevel3)

; This one should be generated after scanning for configuration, using
; something like #any-of? for keywords, but could use a match if allowing
; markup on todo keywords is desirable.
(item . (expr) @OrgKeywordTodo (#eq? @OrgKeywordTodo "TODO"))
(item . (expr) @OrgKeywordDone (#eq? @OrgKeywordDone "DONE"))

; Not sure about this one with the anchors.
(item . (expr)? . (expr "[" "#" @OrgPriority [ "num" "str" ] @OrgPriority "]") @OrgPriorityCookie (#match? @OrgPriorityCookie "\[#.\]"))

; Match cookies in a headline or listitem. If you want the numbers
; differently highlighted from the borders, add a capture name to "num".
; ([ (item) (itemtext) ] (expr "[" "num"? @OrgCookieNum "/" "num"? @OrgCookieNum "]" ) @OrgProgressCookie (#match? @OrgProgressCookie "^\[\d*/\d*\]$"))
; ([ (item) (itemtext) ] (expr "[" "num"? @OrgCookieNum "%" "]" ) @OrgPercentCookie (#match? @OrgPercentCookie "^\[\d*%\]$"))

(tag_list (tag) @OrgTag) @OrgTagList

(property_drawer) @OrgPropertyDrawer

; Properties are :name: vale, so to color the ':' we can either add them
; directly, or highlight the property separately from the name and value. If
; priorities are set properly, it should be simple to achieve.
(property name: (expr) @OrgPropertyName (value)? @OrgPropertyValue) @OrgProperty

; Simple examples, but can also match (day), (date), (time), etc.
(timestamp "[") @OrgTimestampInactive
(timestamp "<"
 (day)? @OrgTimestampDay
 (date)? @OrgTimestampDate
 (time)? @OrgTimestampTime
 (repeat)? @OrgTimestampRepeat
 (delay)? @OrgTimestampDelay
 ) @OrgTimestampActive

; Like OrgProperty, easy to choose how the '[fn:LABEL] DESCRIPTION' are highlighted
(fndef label: (expr) @OrgFootnoteLabel (description) @OrgFootnoteDescription) @OrgFootnoteDefinition

; Again like OrgProperty to change the styling of '#+' and ':'. Note that they
; can also be added in the query directly as anonymous nodes to style differently.
(directive name: (expr) @OrgDirectiveName (value)? @OrgDirectiveValue) @OrgDirective

(comment) @OrgComment

; At the moment, these three elements use one regex for the whole name.
; So (name) -> :name:, ideally this will not be the case, so it follows the
; patterns listed above, but that's the current status. Conflict issues.
(drawer name: (expr) @OrgDrawerName (contents)? @OrgDrawerContents) @OrgDrawer
(block name: (expr) @OrgBlockName (contents)? @OrgBlockContents) @OrgBlock
(dynamic_block name: (expr) @OrgDynamicBlockName (contents)? @OrgDynamicBlockContents) @OrgDynamicBlock

; Can match different styles with a (#match?) or (#eq?) predicate if desired
(bullet) @OrgListBullet

; Get different colors for different statuses as follows
(checkbox) @OrgCheckbox
(checkbox status: (expr "-") @OrgCheckInProgress)
(checkbox status: (expr "str") @OrgCheckDone (#any-of? @OrgCheckDone "x" "X"))
(checkbox status: (expr) @Error (#not-any-of? @Error "x" "X" "-"))

; If you want the ruler one color and the separators a different color,
; something like this would do it:
; (hr "|" @OrgTableHRBar) @OrgTableHorizontalRuler
(hr) @OrgTableHorizontalRuler

; Can do all sorts of fun highlighting here..
(cell (contents . (expr "=")) @OrgCellFormula (#match? @OrgCellFormula "^\d+([.,]\d+)*$"))

; Dollars, floats, etc. Strings.. all options to play with
(cell (contents . (expr "num") @OrgCellNumber (#match? @OrgCellNumber "^\d+([.,]\d+)*$") .))
)TSQ";

static const char *kHighlightsMarkdown = R"TSQ(
;From nvim-treesitter/nvim-treesitter
; Per-level heading captures (@text.title.1 .. @text.title.6) so H1..H6
; can each get a distinct highlight-group color -- the grammar aliases
; all six ATX heading productions to one visible "atx_heading" node type,
; but each still has its own distinct marker child node type
; (atx_h1_marker .. atx_h6_marker) immediately preceding (inline), so a
; separate pattern per level is enough; no predicate needed. No flat
; @text.title fallback pattern is kept alongside these -- a heading node
; matches exactly one of the six marker types, so a generic
; "(atx_heading (inline) @text.title)" pattern would double-match every
; heading (once here, once there) and make which color wins
; non-deterministic (both captures would land on the identical span).
(atx_heading (atx_h1_marker) (inline) @text.title.1)
(atx_heading (atx_h2_marker) (inline) @text.title.2)
(atx_heading (atx_h3_marker) (inline) @text.title.3)
(atx_heading (atx_h4_marker) (inline) @text.title.4)
(atx_heading (atx_h5_marker) (inline) @text.title.5)
(atx_heading (atx_h6_marker) (inline) @text.title.6)

(setext_heading
  (paragraph) @text.title.1
  (setext_h1_underline))

(setext_heading
  (paragraph) @text.title.2
  (setext_h2_underline))

[
  (atx_h1_marker)
  (atx_h2_marker)
  (atx_h3_marker)
  (atx_h4_marker)
  (atx_h5_marker)
  (atx_h6_marker)
  (setext_h1_underline)
  (setext_h2_underline)
] @punctuation.special

[
  (link_title)
  (indented_code_block)
  (fenced_code_block)
] @text.literal

(fenced_code_block_delimiter) @punctuation.delimiter

(code_fence_content) @none

(link_destination) @text.uri

(link_label) @text.reference

[
  (list_marker_plus)
  (list_marker_minus)
  (list_marker_star)
  (list_marker_dot)
  (list_marker_parenthesis)
  (thematic_break)
] @punctuation.special

[
  (block_continuation)
  (block_quote_marker)
] @punctuation.special

(backslash_escape) @string.escape
)TSQ";

static const char *kHighlightsMarkdownInline = R"TSQ(
; From nvim-treesitter/nvim-treesitter
[
  (code_span)
  (link_title)
] @text.literal

[
  (emphasis_delimiter)
  (code_span_delimiter)
] @punctuation.delimiter

(emphasis) @text.emphasis

(strong_emphasis) @text.strong

[
  (link_destination)
  (uri_autolink)
] @text.uri

[
  (link_label)
  (link_text)
  (image_description)
] @text.reference

[
  (backslash_escape)
  (hard_line_break)
] @string.escape

(image
  [
    "!"
    "["
    "]"
    "("
    ")"
  ] @punctuation.delimiter)

(inline_link
  [
    "["
    "]"
    "("
    ")"
  ] @punctuation.delimiter)

(shortcut_link
  [
    "["
    "]"
  ] @punctuation.delimiter)

; NOTE: extension not enabled by default
; (wiki_link ["[" "|" "]"] @punctuation.delimiter)
)TSQ";

static const char *kHighlightsHcl = R"TSQ(
; highlights.scm
[
  "!"
  "\*"
  "/"
  "%"
  "\+"
  "-"
  ">"
  ">="
  "<"
  "<="
  "=="
  "!="
  "&&"
  "||"
] @operator

[
  "{"
  "}"
  "["
  "]"
  "("
  ")"
] @punctuation.bracket

[
  "."
  ".*"
  ","
  "[*]"
] @punctuation.delimiter

[
  (ellipsis)
  "\?"
  "=>"
] @punctuation.special

[
  ":"
  "="
] @none

[
  "for"
  "endfor"
  "in"
] @keyword.repeat

[
  "if"
  "else"
  "endif"
] @keyword.conditional

[
  (quoted_template_start) ; "
  (quoted_template_end) ; "
  (template_literal) ; non-interpolation/directive content
] @string

[
  (heredoc_identifier) ; END
  (heredoc_start) ; << or <<-
] @punctuation.delimiter

[
  (template_interpolation_start) ; ${
  (template_interpolation_end) ; }
  (template_directive_start) ; %{
  (template_directive_end) ; }
  (strip_marker) ; ~
] @punctuation.special

(numeric_lit) @number

(bool_lit) @boolean

(null_lit) @constant

(comment) @comment @spell

(identifier) @variable

(body
  (block
    (identifier) @keyword))

(body
  (block
    (body
      (block
        (identifier) @type))))

(function_call
  (identifier) @function)

(attribute
  (identifier) @variable.member)

; { key: val }
;
; highlight identifier keys as though they were block attributes
(object_elem
  key: (expression
    (variable_expr
      (identifier) @variable.member)))

; var.foo, data.bar
;
; first element in get_attr is a variable.builtin or a reference to a variable.builtin
(expression
  (variable_expr
    (identifier) @variable.builtin)
  (get_attr
    (identifier) @variable.member))
)TSQ";

static const char *kHighlightsKotlin = R"TSQ(
; Identifiers
(simple_identifier) @variable

; `it` keyword inside lambdas
; FIXME: This will highlight the keyword outside of lambdas since tree-sitter
;        does not allow us to check for arbitrary nestation
((simple_identifier) @variable.builtin
  (#eq? @variable.builtin "it"))

; `field` keyword inside property getter/setter
; FIXME: This will highlight the keyword outside of getters and setters
;        since tree-sitter does not allow us to check for arbitrary nestation
((simple_identifier) @variable.builtin
  (#eq? @variable.builtin "field"))

[
  "this"
  "super"
  "this@"
  "super@"
] @variable.builtin

; NOTE: for consistency with "super@"
(super_expression
  "@" @variable.builtin)

(class_parameter
  (simple_identifier) @variable.member)

; NOTE: temporary fix for treesitter bug that causes delay in file opening
;(class_body
;  (property_declaration
;    (variable_declaration
;      (simple_identifier) @variable.member)))
; id_1.id_2.id_3: `id_2` and `id_3` are assumed as object properties
(_
  (navigation_suffix
    (simple_identifier) @variable.member))

; SCREAMING CASE identifiers are assumed to be constants
((simple_identifier) @constant
  (#lua-match? @constant "^[A-Z][A-Z0-9_]*$"))

(_
  (navigation_suffix
    (simple_identifier) @constant
    (#lua-match? @constant "^[A-Z][A-Z0-9_]*$")))

(enum_entry
  (simple_identifier) @constant)

(type_identifier) @type

; '?' operator, replacement for Java @Nullable
(nullable_type) @punctuation.special

(type_alias
  (type_identifier) @type.definition)

((type_identifier) @type.builtin
  (#any-of? @type.builtin
    "Byte" "Short" "Int" "Long" "UByte" "UShort" "UInt" "ULong" "Float" "Double" "Boolean" "Char"
    "String" "Array" "ByteArray" "ShortArray" "IntArray" "LongArray" "UByteArray" "UShortArray"
    "UIntArray" "ULongArray" "FloatArray" "DoubleArray" "BooleanArray" "CharArray" "Map" "Set"
    "List" "EmptyMap" "EmptySet" "EmptyList" "MutableMap" "MutableSet" "MutableList"))

(package_header
  "package" @keyword
  .
  (identifier
    (simple_identifier) @module))

(import_header
  "import" @keyword.import)

(wildcard_import) @character.special

; The last `simple_identifier` in a `import_header` will always either be a function
; or a type. Classes can appear anywhere in the import path, unlike functions
(import_header
  (identifier
    (simple_identifier) @type @_import)
  (import_alias
    (type_identifier) @type.definition)?
  (#lua-match? @_import "^[A-Z]"))

(import_header
  (identifier
    (simple_identifier) @function @_import .)
  (import_alias
    (type_identifier) @function)?
  (#lua-match? @_import "^[a-z]"))

(label) @label

; Function definitions
(function_declaration
  (simple_identifier) @function)

(getter
  "get" @function.builtin)

(setter
  "set" @function.builtin)

(primary_constructor) @constructor

(secondary_constructor
  "constructor" @constructor)

(constructor_invocation
  (user_type
    (type_identifier) @constructor))

(anonymous_initializer
  "init" @constructor)

(parameter
  (simple_identifier) @variable.parameter)

(parameter_with_optional_type
  (simple_identifier) @variable.parameter)

; lambda parameters
(lambda_literal
  (lambda_parameters
    (variable_declaration
      (simple_identifier) @variable.parameter)))

; Function calls
; function()
(call_expression
  .
  (simple_identifier) @function.call)

; ::function
(callable_reference
  .
  (simple_identifier) @function.call)

; object.function() or object.property.function()
(call_expression
  (navigation_expression
    (navigation_suffix
      (simple_identifier) @function.call) .))

(call_expression
  .
  (simple_identifier) @function.builtin
  (#any-of? @function.builtin
    "arrayOf" "arrayOfNulls" "byteArrayOf" "shortArrayOf" "intArrayOf" "longArrayOf" "ubyteArrayOf"
    "ushortArrayOf" "uintArrayOf" "ulongArrayOf" "floatArrayOf" "doubleArrayOf" "booleanArrayOf"
    "charArrayOf" "emptyArray" "mapOf" "setOf" "listOf" "emptyMap" "emptySet" "emptyList"
    "mutableMapOf" "mutableSetOf" "mutableListOf" "print" "println" "error" "TODO" "run"
    "runCatching" "repeat" "lazy" "lazyOf" "enumValues" "enumValueOf" "assert" "check"
    "checkNotNull" "require" "requireNotNull" "with" "suspend" "synchronized"))

; Literals
[
  (line_comment)
  (multiline_comment)
] @comment @spell

((multiline_comment) @comment.documentation
  (#lua-match? @comment.documentation "^/[*][*][^*].*[*]/$"))

(shebang_line) @keyword.directive

(real_literal) @number.float

[
  (integer_literal)
  (long_literal)
  (hex_literal)
  (bin_literal)
  (unsigned_literal)
] @number

[
  ; tree-sitter-kotlin 0.3.8 (flake.nix's) has no null_literal node --
  ; `null` is a bare token there.
  "null"
  ; should be highlighted the same as booleans
  (boolean_literal)
] @boolean

(character_literal) @character

(string_literal) @string

; NOTE: Escapes not allowed in multi-line strings
(character_literal
  (character_escape_seq) @string.escape)

; There are 3 ways to define a regex
;    - "[abc]?".toRegex()
(call_expression
  (navigation_expression
    (string_literal) @string.regexp
    (navigation_suffix
      ((simple_identifier) @_function
        (#eq? @_function "toRegex")))))

;    - Regex("[abc]?")
(call_expression
  ((simple_identifier) @_function
    (#eq? @_function "Regex"))
  (call_suffix
    (value_arguments
      (value_argument
        (string_literal) @string.regexp))))

;    - Regex.fromLiteral("[abc]?")
(call_expression
  (navigation_expression
    ((simple_identifier) @_class
      (#eq? @_class "Regex"))
    (navigation_suffix
      ((simple_identifier) @_function
        (#eq? @_function "fromLiteral"))))
  (call_suffix
    (value_arguments
      (value_argument
        (string_literal) @string.regexp))))

; Keywords
(type_alias
  "typealias" @keyword)

(companion_object
  "companion" @keyword)

[
  (class_modifier)
  (member_modifier)
  (function_modifier)
  (property_modifier)
  (platform_modifier)
  (variance_modifier)
  (parameter_modifier)
  (visibility_modifier)
  (reification_modifier)
  (inheritance_modifier)
] @keyword.modifier

[
  "val"
  "var"
  ;	"typeof" ; NOTE: It is reserved for future use
] @keyword

[
  "enum"
  "class"
  "object"
  "interface"
] @keyword.type

[
  "return"
  "return@"
] @keyword.return

"suspend" @keyword.coroutine

"fun" @keyword.function

[
  "if"
  "else"
  "when"
] @keyword.conditional

[
  "for"
  "do"
  "while"
  "continue"
  "continue@"
  "break"
  "break@"
] @keyword.repeat

[
  "try"
  "catch"
  "throw"
  "finally"
] @keyword.exception

(annotation
  "@" @attribute
  (use_site_target)? @attribute)

(annotation
  (user_type
    (type_identifier) @attribute))

(annotation
  (constructor_invocation
    (user_type
      (type_identifier) @attribute)))

(file_annotation
  "@" @attribute
  "file" @attribute
  ":" @attribute)

(file_annotation
  (user_type
    (type_identifier) @attribute))

(file_annotation
  (constructor_invocation
    (user_type
      (type_identifier) @attribute)))

; Operators & Punctuation
[
  "!"
  "!="
  "!=="
  "="
  "=="
  "==="
  ">"
  ">="
  "<"
  "<="
  "||"
  "&&"
  "+"
  "++"
  "+="
  "-"
  "--"
  "-="
  "*"
  "*="
  "/"
  "/="
  "%"
  "%="
  "?."
  "?:"
  "!!"
  "is"
  "!is"
  "in"
  "!in"
  "as"
  "as?"
  ".."
  "->"
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  "."
  ","
  ";"
  ":"
  "::"
] @punctuation.delimiter

(super_expression
  [
    "<"
    ">"
  ] @punctuation.delimiter)

(type_arguments
  [
    "<"
    ">"
  ] @punctuation.delimiter)

(type_parameters
  [
    "<"
    ">"
  ] @punctuation.delimiter)

; NOTE: `interpolated_identifier`s can be highlighted in any way
(string_literal
  "$" @punctuation.special
  (interpolated_identifier) @none @variable)

(string_literal
  "${" @punctuation.special
  (interpolated_expression) @none
  "}" @punctuation.special)
)TSQ";

// nvim-treesitter v0.9.1's queries/verilog/highlights.scm (Apache-2.0), the
// last one written for tree-sitter/tree-sitter-verilog -- the grammar
// nixpkgs packages. Later nvim-treesitter moved verilog to a different
// (SystemVerilog) grammar, and the query vendored here before came from
// that, so most of it named nodes this grammar doesn't have. Minus the
// "config"/"endconfig" keywords, which this grammar has no tokens for.
static const char *kHighlightsVerilog = R"TSQ(
; Keywords

[
  ; block delimiters
  (module_keyword)
  "endmodule"
  "program"
  "endprogram"
  "class"
  "endclass"
  "interface"
  "endinterface"
  "package"
  "endpackage"
  "checker"
  "endchecker"

  "pure"
  "virtual"
  "extends"
  "implements"
  "super"
  (class_item_qualifier)

  "parameter"
  "localparam"
  "defparam"
  "assign"
  "typedef"
  "modport"
  "fork"
  "join"
  "join_none"
  "join_any"
  "default"
  "break"
  "assert"
  "tagged"
  "extern"
  (unique_priority)
] @keyword

[
  "function"
  "endfunction"

  "task"
  "endtask"
] @keyword.function

"return" @keyword.return

[
  "begin"
  "end"
] @label

[
  (always_keyword)
  "generate"
  "for"
  "foreach"
  "repeat"
  "forever"
  "initial"
  "while"
] @repeat

[
  "if"
  "else"
  (case_keyword)
  "endcase"
] @conditional

(comment) @comment

(include_compiler_directive) @constant.macro
(package_import_declaration
 "import" @include)

(package_import_declaration
 (package_import_item
  (package_identifier
   (simple_identifier) @constant)))

(text_macro_identifier
 (simple_identifier) @constant.macro)

(package_scope
 (package_identifier
  (simple_identifier) @constant))

(package_declaration
 (package_identifier
  (simple_identifier) @constant))

(parameter_port_list
 "#" @constructor)

[
  "="
  "-"
  "+"
  "/"
  "*"
  "^"
  "&"
  "|"
  "&&"
  "||"
  ":"
  "{"
  "}"
  "'{"
  "<="
  "@"
  "=="
  "!="
  "==="
  "!=="
  "-:"
  "<"
  ">"
  ">="
  "%"
  ">>"
  "<<"
  "|="
  (unary_operator)
  (inc_or_dec_operator)
] @operator

[
  "or"
  "and"
] @keyword.operator

(cast
 ["'" "(" ")"] @operator)

(edge_identifier) @attribute

(port_direction) @label
(port_identifier
 (simple_identifier) @variable)

[
  (net_type)
  (integer_vector_type)
  (integer_atom_type)
] @type.builtin

[
  "signed"
  "unsigned"
] @type.qualifier

(data_type
 (simple_identifier) @type)

(method_call_body
  (method_identifier) @field)

(interface_identifier
 (simple_identifier) @type)

(modport_identifier
 (modport_identifier
  (simple_identifier) @field))

(net_port_type1
 (simple_identifier) @type)

[
  (double_quoted_string)
  (string_literal)
] @string

[
  (default_nettype_compiler_directive)
  (timescale_compiler_directive)
] @preproc

(include_compiler_directive) @include

; begin/end label
(seq_block
 (simple_identifier) @comment)

[
 ";"
 "::"
 ","
 "."
] @punctuation.delimiter


(default_nettype_compiler_directive
 (default_nettype_value) @string)

(text_macro_identifier
 (simple_identifier) @constant)

(module_declaration
 (module_header
  (simple_identifier) @constructor))

(class_constructor_declaration
 "new" @constructor)

(parameter_identifier
 (simple_identifier) @parameter)

[
  (integral_number)
  (unsigned_number)
  (unbased_unsized_literal)
] @number

(time_unit) @attribute

(checker_instantiation
 (checker_identifier
  (simple_identifier) @constructor))

(module_instantiation
 (simple_identifier) @constructor)

(name_of_instance
 (instance_identifier
  (simple_identifier) @variable))

(interface_port_declaration
 (interface_identifier
  (simple_identifier) @type))

(net_declaration
 (simple_identifier) @type)

(lifetime) @label

(function_identifier 
 (function_identifier 
  (simple_identifier) @function))

(function_subroutine_call 
 (subroutine_call
  (tf_call
   (simple_identifier) @function)))

(function_subroutine_call 
 (subroutine_call
  (system_tf_call
   (system_tf_identifier) @function.builtin)))

(task_identifier
 (task_identifier
  (simple_identifier) @method))

;;TODO: fixme
;(assignment_pattern_expression
 ;(assignment_pattern
  ;(parameter_identifier) @field))

(type_declaration
  (data_type ["packed"] @type.qualifier))

(struct_union) @type

[
  "enum"
] @type

(enum_name_declaration
 (enum_identifier
  (simple_identifier) @constant))

(type_declaration
 (simple_identifier) @type)

[
  (integer_atom_type)
  (non_integer_type)
  "genvar"
] @type.builtin

(struct_union_member
 (list_of_variable_decl_assignments
  (variable_decl_assignment
   (simple_identifier) @field)))

(member_identifier
 (simple_identifier) @field)

(struct_union_member
 (data_type_or_void
  (data_type
   (simple_identifier) @type)))

(type_declaration
 (simple_identifier) @type)

(generate_block_identifier) @comment

[
  "["
  "]"
  "("
  ")"
] @punctuation.bracket

(ERROR) @error
)TSQ";

static const char *kHighlightsGraphql = R"TSQ(
; Types
;------
(scalar_type_definition
  (name) @type)

(object_type_definition
  (name) @type)

(interface_type_definition
  (name) @type)

(union_type_definition
  (name) @type)

(enum_type_definition
  (name) @type)

(input_object_type_definition
  (name) @type)

(scalar_type_extension
  (name) @type)

(object_type_extension
  (name) @type)

(interface_type_extension
  (name) @type)

(union_type_extension
  (name) @type)

(enum_type_extension
  (name) @type)

(input_object_type_extension
  (name) @type)

(named_type
  (name) @type)

; Directives
;-----------
(directive_definition
  "@" @attribute
  (name) @attribute)

(directive) @attribute

; Properties
;-----------
(field
  (name) @property)

(field
  (alias
    (name) @property))

(field_definition
  (name) @property)

(object_value
  (object_field
    (name) @property))

(enum_value
  (name) @property)

; Variable Definitions and Arguments
;-----------------------------------
(operation_definition
  (name) @variable)

(fragment_name
  (name) @variable)

(input_fields_definition
  (input_value_definition
    (name) @variable.parameter))

(argument
  (name) @variable.parameter)

(arguments_definition
  (input_value_definition
    (name) @variable.parameter))

(variable_definition
  (variable) @variable.parameter)

(argument
  (value
    (variable) @variable))

; Constants
;----------
(string_value) @string

(int_value) @number

(float_value) @number.float

(boolean_value) @boolean

; Literals
;---------
(description
  (string_value) @string.documentation @spell)

(comment) @comment @spell

(directive_location
  (executable_directive_location) @type.builtin)

(directive_location
  (type_system_directive_location) @type.builtin)

; Keywords
;----------
[
  "query"
  "mutation"
  "subscription"
  "fragment"
  "scalar"
  "input"
  "extend"
  "directive"
  "schema"
  "on"
  "repeatable"
  "implements"
] @keyword

[
  "enum"
  "union"
  "type"
  "interface"
] @keyword.type

; Punctuation
;------------
[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

"=" @operator

"|" @punctuation.delimiter

"&" @punctuation.delimiter

":" @punctuation.delimiter

"..." @punctuation.special

"!" @punctuation.special
)TSQ";

static const char *kHighlightsCpp = R"TSQ(
(identifier) @variable

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z\\d_]*$"))

"break" @keyword
"case" @keyword
"const" @keyword
"continue" @keyword
"default" @keyword
"do" @keyword
"else" @keyword
"enum" @keyword
"extern" @keyword
"for" @keyword
"if" @keyword
"inline" @keyword
"return" @keyword
"sizeof" @keyword
"static" @keyword
"struct" @keyword
"switch" @keyword
"typedef" @keyword
"union" @keyword
"volatile" @keyword
"while" @keyword

"#define" @keyword
"#elif" @keyword
"#else" @keyword
"#endif" @keyword
"#if" @keyword
"#ifdef" @keyword
"#ifndef" @keyword
"#include" @keyword
(preproc_directive) @keyword

"--" @operator
"-" @operator
"-=" @operator
"->" @operator
"=" @operator
"!=" @operator
"*" @operator
"&" @operator
"&&" @operator
"+" @operator
"++" @operator
"+=" @operator
"<" @operator
"==" @operator
">" @operator
"||" @operator

"." @delimiter
";" @delimiter

(string_literal) @string
(system_lib_string) @string

(null) @constant
(number_literal) @number
(char_literal) @number

(field_identifier) @property
(statement_identifier) @label
(type_identifier) @type
(primitive_type) @type
(sized_type_specifier) @type

(call_expression
  function: (identifier) @function)
(call_expression
  function: (field_expression
    field: (field_identifier) @function))
(function_declarator
  declarator: (identifier) @function)
(preproc_function_def
  name: (identifier) @function.special)

(comment) @comment

; Functions

(call_expression
  function: (qualified_identifier
    name: (identifier) @function))

(template_function
  name: (identifier) @function)

(template_method
  name: (field_identifier) @function)

(template_function
  name: (identifier) @function)

(function_declarator
  declarator: (qualified_identifier
    name: (identifier) @function))

(function_declarator
  declarator: (field_identifier) @function)

; Types

((namespace_identifier) @type
 (#match? @type "^[A-Z]"))

(auto) @type

; Constants

(this) @variable.builtin
(null "nullptr" @constant)

; Keywords

[
 "catch"
 "class"
 "co_await"
 "co_return"
 "co_yield"
 "constexpr"
 "constinit"
 "consteval"
 "delete"
 "explicit"
 "final"
 "friend"
 "mutable"
 "namespace"
 "noexcept"
 "new"
 "override"
 "private"
 "protected"
 "public"
 "template"
 "throw"
 "try"
 "typename"
 "using"
 "concept"
 "requires"
 "virtual"
] @keyword

; Strings

(raw_string_literal) @string
)TSQ";

// Perl/Fortran/D/Nim/Crystal (added for org-babel src-block syntax
// highlighting -- mep.org_babel_langs supports all five but no grammar
// query existed for them yet). Fortran/D/Nim/Crystal are vendored
// verbatim from each grammar's own upstream queries/highlights.scm, same
// convention as every other dynamically-loaded language above (Crystal's
// own repo keeps its real highlight query under queries/nvim/
// highlights.scm rather than the usual path). Perl is a hand-written
// query, NOT vendored from upstream: the actively-maintained
// tree-sitter-perl/tree-sitter-perl grammar ships no committed
// `src/parser.c` (would need the tree-sitter CLI to generate one -- the
// exact "not vendorable" case this file's own README.md already
// documents for sql/latex/swift), so flake.nix/the justfile's
// fetch-grammars recipe both build the older, simpler
// ganezdragon/tree-sitter-perl instead (which does ship one) --
// deliberately a *different* grammar than the one this query would have
// matched, so it's written directly against ganezdragon's own
// src/node-types.json node names instead of carrying over a query that
// would have silently matched nothing.
static const char *kHighlightsPerl = R"TSQ(
(comments) @comment
(pod_statement) @comment

[
  (string_double_quoted)
  (string_single_quoted)
  (string_qq_quoted)
  (string_q_quoted)
  (heredoc_body_statement)
  (backtick_quoted)
  (command_qx_quoted)
] @string

(escape_sequence) @string.escape

[
  (integer)
  (floating_point)
  (hexadecimal)
  (octal)
  (scientific_notation)
  (version)
] @number

[ (true) (false) ] @boolean

[
  (scalar_variable)
  (array_variable)
  (hash_variable)
  (package_variable)
  (special_scalar_variable)
] @variable

[ "if" "elsif" "unless" "else" "when" ] @conditional

[ "for" "foreach" "while" "until" ] @repeat

[ "my" "our" "local" "state" ] @keyword

"sub" @keyword.function
"return" @keyword.return
[ "use" "no" "require" "import" "package" ] @include

[
  "and" "or" "not" "xor"
  "eq" "ne" "lt" "gt" "le" "ge" "cmp" "isa"
] @keyword.operator

[ "last" "next" "redo" "goto" "continue" ] @keyword

(package_name) @type
(module_name) @type

(function_definition name: (identifier) @function)

(ERROR) @error
)TSQ";

// tree-sitter-fortran 0.5.1's own queries/highlights.scm (the version
// flake.nix's grammar bundle pins): the query vendored here before named
// nodes that grammar lacks (custom_directive, a deallocate_statement
// keyword), so ts_query_new rejected it and .f90 got no highlighting.
static const char *kHighlightsFortran = R"TSQ(
(identifier) @variable
(string_literal) @string
(number_literal) @number
(boolean_literal) @boolean
(comment) @comment

[
 (intrinsic_type)
 "allocatable"
 "attributes"
 "device"
 "dimension"
 "endtype"
 "global"
 "grid_global"
 "host"
 "import"
 "in"
 "inout"
 "intent"
 "optional"
 "out"
 "pointer"
 "type"
 "value"
 ] @type

[
 "contains"
 "private"
 "public"
 ] @include

[
 (none)
 "implicit"
 ] @attribute

[
 "endfunction"
 "endprogram"
 "endsubroutine"
 "function"
 "procedure"
 "subroutine"
 ] @keyword.function

[
 (default)
 (procedure_qualifier)
 "abstract"
 "bind"
 "call"
 "class"
 "continue"
 "cycle"
 "endenum"
 "endinterface"
 "endmodule"
 "endprocedure"
 "endprogram"
 "endsubmodule"
 "enum"
 "enumerator"
 "equivalence"
 "exit"
 "extends"
 "format"
 "goto"
 "include"
 "interface"
 "intrinsic"
 "non_intrinsic"
 "module"
 "namelist"
 "only"
 "parameter"
 "print"
 "procedure"
 "program"
 "read"
 "stop"
 "submodule"
 "use"
 "write"
 ] @keyword

"return" @keyword.return

[
 "else"
 "elseif"
 "elsewhere"
 "endif"
 "endwhere"
 "if"
 "then"
 "where"
 ] @conditional

[
 "do"
 "enddo"
 "forall"
 "while"
 ] @repeat

[
 "*"
 "+"
 "-"
 "/"
 "="
 "<"
 ">"
 "<="
 ">="
 "=="
 "/="
 ] @operator

[
 "\\.and\\."
 "\\.or\\."
 "\\.lt\\."
 "\\.gt\\."
 "\\.ge\\."
 "\\.le\\."
 "\\.eq\\."
 "\\.eqv\\."
 "\\.neqv\\."
 ] @keyword.operator

;; Brackets
[
 "("
 ")"
 "["
 "]"
 "<<<"
 ">>>"
 ] @punctuation.bracket

;; Delimiter
[
 "::"
 ","
 "%"
 ] @punctuation.delimiter

(parameters
  (identifier) @parameter)

(program_statement
  (name) @namespace)

(module_statement
  (name) @namespace)

(submodule_statement
  (module_name) (name) @namespace)

(function_statement
  (name) @function)

(subroutine_statement
  (name) @function)

(module_procedure_statement
  (name) @function)

(end_program_statement
  (name) @namespace)

(end_module_statement
  (name) @namespace)

(end_submodule_statement
  (name) @namespace)

(end_function_statement
  (name) @function)

(end_subroutine_statement
  (name) @function)

(end_module_procedure_statement
  (name) @function)

(subroutine_call
  (identifier) @function)

(keyword_argument
  name: (identifier) @keyword)

(derived_type_member_expression
  (type_member) @property)
)TSQ";

static const char *kHighlightsD = R"TSQ(
; highlights.scm
;
; Highlighting queries for D code for use by Tree-Sitter.
;
; Copyright 2024 Garrett D'Amore
;
; Distributed under the MIT License.
; (See accompanying file LICENSE.txt or https://opensource.org/licenses/MIT)
; SPDX-License-Identifier: MIT

(string_literal) @string
(int_literal) @number
(float_literal) @number
(char_literal) @number
(identifier) @variable
(at_attribute) @property
(htmlentity) @string.special
(escape_sequence) @string.escape

[
	(lazy)
	(align)
	(extern)
	(static)
	(abstract)
	(final)
	(override)
	(synchronized)
	(auto)
	(scope)
	(gshared)
	(ref)
	(deprecated)
	(nothrow)
	(pure)
	(type_ctor)
] @keyword.storage

(parameter_attribute (return) @keyword.storage)
(parameter_attribute (in) @keyword.storage)
(parameter_attribute (out) @keyword.storage)

(function_declaration (identifier) @function)

(call_expression (identifier) @function)
(call_expression (type (template_instance (identifier) @function)))
(template_arguments (identifier) @variable.parameter)

(named_argument (identifier) @variable.parameter)

[
    (abstract)
    (alias)
    (align)
    (asm)
    (assert)
    (auto)
    (cast)
    (class)
    (const)
    (debug)
    (delegate)
    (delete)
    (deprecated)
    (enum)
    (export)
    (extern)
    (final)
    (function)
    (immutable)
    (import)
    (in)
    (inout)
    (interface)
    (invariant)
    (is)
    (lazy)
    ; "macro" - obsolete
    (mixin)
    (module)
    (new)
    (nothrow)
    (out)
    (override)
    (package)
    (pragma)
    (private)
    (protected)
    (public)
    (pure)
    (ref)
    (scope)
    (shared)
    (static)
    (struct)
    (super)
    (synchronized)
    (template)
    (this)
    (throw)
    (typeid)
    (typeof)
    (union)
    (unittest)
    (version)
    (with)
    (gshared)
    (traits)
    (vector)
    (parameters_)
] @keyword

[
    (break)
    (case)
    (catch)
    (continue)
    (do)
    (default)
    (finally)
    (else)
    (for)
    (foreach)
    (foreach_reverse)
    (goto)
    (if)
    (switch)
    (try)
    (return)
    (while)
] @keyword.control

[
    (not_in)
    (not_is)
    "/="
    "/"
    ".."
    "..."
    "&"
    "&="
    "&&"
    "|"
    "|="
    "||"
    "-"
    "-="
    "--"
    "+"
    "+="
    "++"
    "<"
    "<="
    "<<"
    "<<="
    ">"
    ">="
    ">>="
    ">>>="
    ">>"
    ">>>"
    "!"
    "!="
    "?"
    "$"
    "="
    "=="
    "*"
    "*="
    "%"
    "%="
    "^"
    "^="
    "^^"
    "^^="
    "~"
    "~="
    "@"
    "=>"
] @operator

[
    ";"
    "."
    ":"
    ","
] @punctuation.delimiter

[
    "("
    ")"
    "["
    "]"
    "{"
    "}"
] @punctuation.bracket

; The delimiters of an interpolation expression inside an interpolated string.
; Listed after the bracket rule above so the closing ")" is captured here rather
; than as a plain bracket; this keeps the opening "$(" and closing ")" the same
; color instead of the opener inheriting the surrounding string color.
(interpolation_expression
    "$(" @punctuation.special
    ")" @punctuation.special)

[
    (null)
    (true)
    (false)
] @constant.language

(special_keyword) @constant.language

(directive) @keyword.directive
(shebang) @keyword.directive

(comment) @comment

[
    (void)
    (bool)
    (byte)
    (ubyte)
    (char)
    (short)
    (ushort)
    (wchar)
    (dchar)
    (int)
    (uint)
    (long)
    (ulong)
    (real)
    (double)
    (float)
    (size_t)
    (ptrdiff_t)
    (string)
    (cstring)
    (wstring)
    (noreturn)
] @type.builtin

[
    (cent)
    (ucent)
    (ireal)
    (idouble)
    (ifloat)
    (creal)
    (double)
    (cfloat)
] @type.deprecated

(label (identifier) @label)
(goto_statement (goto) @keyword.control (identifier) @label)

; this covers other cases where the identifier can only
; be a type (such as in an is-expression on a constraint)
(type (identifier) @type)

; these are listed last, because they override keyword queries
(identity_expression (in) @operator)
(identity_expression (is) @operator)

; everything after __EOF_ is plain text
(end_file) @text
)TSQ";

static const char *kHighlightsNim = R"TSQ(
; SPDX-FileCopyrightText: 2023 Leorize <leorize+oss@disroot.org>
; SPDX-License-Identifier: MPL-2.0

; Punctuations
[ "." ";" "," ":" ] @punctuation.delimiter
[ "(" ")" "[" "]" "{" "}" "{." ".}" ] @punctuation.bracket

; Operator by default, but could be overriden
[ "=" ] @operator

; Special
(blank_identifier) @variable.builtin

; Calls
(call
  function: [
    (identifier) @function.call
    (dot_expression
      right: (identifier) @function.call)
  ])
(generalized_string
  function: [
    (identifier) @function.call
    (dot_expression
      right: (identifier) @function.call)
  ])

; Declarations
(exported_symbol "*" @type.qualifier)
(_ "=" @punctuation.delimiter [body: (_) value: (_)])
(proc_declaration name: (_) @function)
(func_declaration name: (_) @function)
(converter_declaration name: (_) @function)
(method_declaration name: (_) @method)
(template_declaration name: (_) @function.macro)
(macro_declaration name: (_) @function.macro)
(symbol_declaration name: (_) @variable)
(parameter_declaration
  (symbol_declaration_list
    (symbol_declaration name: (_) @parameter)))
(_
  [
    type: [
      (type_expression (identifier))
      (type_expression (accent_quoted (identifier)))
    ] @type
    return_type: [
      (type_expression (identifier))
      (type_expression (accent_quoted (identifier)))
    ] @type
  ])

; Exceptions
[
  "try"
  "except"
  "finally"
  "raise"
] @exception

(except_branch values: (expression_list
  [
    (identifier) @type
    (infix_expression
      left: (identifier) @type
      operator: "as"
      right: (identifier) @variable)
  ]))

; Expressions
(dot_expression
  right: (identifier) @field)

; Literal/comments
[
  (comment)
  (block_comment)
] @comment

[
  (documentation_comment)
  (block_documentation_comment)
] @comment.documentation

(interpreted_string_literal) @string
(long_string_literal) @string
(raw_string_literal) @string
(generalized_string) @string
(char_literal) @character
(escape_sequence) @string.escape
(integer_literal) @number
(float_literal) @float
(custom_numeric_literal) @number
(nil_literal) @constant.builtin

; Keyword
[
  "if"
  "when"
  "case"
  "elif"
  "else"
] @conditional

(of_branch "of" @conditional)

[
  "import"
  "include"
  "export"
] @include

(import_from_statement "from" @include)
(except_clause "except" @include)

[
  "for"
  "while"
  "continue"
  "break"
] @repeat

(for "in" @repeat)

[
  "macro"
  "template"
  "const"
  "let"
  "var"
  "asm"
  "bind"
  "block"
  "concept"
  "defer"
  "discard"
  "distinct"
  "do"
  "enum"
  "mixin"
  "nil"
  "object"
  "out"
  "ptr"
  "ref"
  "static"
  "tuple"
  "type"
] @keyword

[
  "proc"
  "func"
  "method"
  "converter"
  "iterator"
] @keyword.function

[
  "and"
  "or"
  "xor"
  "not"
  "div"
  "mod"
  "shl"
  "shr"
  "from"
  "as"
  "of"
  "in"
  "notin"
  "is"
  "isnot"
  "cast"
] @keyword.operator

[
  "return"
  "yield"
] @keyword.return

; Operators
(infix_expression operator: _ @operator)
(prefix_expression operator: _ @operator)

)TSQ";

// MATLAB/Octave (.m) -- acristoffers/tree-sitter-matlab's own
// queries/neovim/highlights.scm (nixpkgs' tree-sitter-matlab ships it),
// minus nvim's @spell capture. The grammar parses Octave scripts too.
static const char *kHighlightsMatlab = R"TSQ(
; Includes

((command_name) @include
  (#eq? @include "import"))

; Keywords

[
  "arguments"
  "classdef"
  "end"
  "enumeration"
  "events"
  "global"
  "methods"
  "persistent"
  "properties"
] @keyword

; Conditionals

(if_statement [ "if" "end" ] @conditional)
(elseif_clause "elseif" @conditional)
(else_clause "else" @conditional)
(switch_statement [ "switch" "end" ] @conditional)
(case_clause "case" @conditional)
(otherwise_clause "otherwise" @conditional)
(break_statement) @conditional

; Repeats

(for_statement [ "for" "parfor" "end" ] @repeat)
(while_statement [ "while" "end" ] @repeat)
(continue_statement) @repeat

; Exceptions

(try_statement [ "try" "end" ] @exception)
(catch_clause "catch" @exception)

; Variables

(identifier) @variable

; Constants

(events (identifier) @constant)
(attribute (identifier) @constant)

"~" @constant.builtin

; Fields/Properties

(field_expression field: (identifier) @field)

(superclass "." (identifier) @property)

(property_name "." (identifier) @property)

(property name: (identifier) @property)

; Types

(class_definition name: (identifier) @type)

(attributes (identifier) @constant)

(enum . (identifier) @type)

((identifier) @type
  (#lua-match? @type "^_*[A-Z][a-zA-Z0-9_]+$"))

; Functions

(function_definition
  "function" @keyword.function
  name: (identifier) @function
  [ "end" "endfunction" ]? @keyword.function)

(function_signature name: (identifier) @function)

(function_call
  name: (identifier) @function.call)

(handle_operator (identifier) @function)

(validation_functions (identifier) @function)

(command (command_name) @function.call)
(command_argument) @parameter

(return_statement) @keyword.return

; Parameters

(function_arguments (identifier) @parameter)

; Punctuation

[ ";" "," "." ] @punctuation.delimiter

[ "(" ")" "[" "]" "{" "}" ] @punctuation.bracket

; Operators

[
  "+"
  ".+"
  "-"
  ".*"
  "*"
  ".*"
  "/"
  "./"
  "\\"
  ".\\"
  "^"
  ".^"
  "'"
  ".'"
  "|"
  "&"
  "?"
  "@"
  "<"
  "<="
  ">"
  ">="
  "=="
  "~="
  "="
  "&&"
  "||"
  ":"
] @operator

; Literals

(string) @string

(escape_sequence) @string.escape
(formatting_sequence) @string.special

(number) @number

((identifier) @boolean
  (#eq? @boolean "true"))
((identifier) @boolean
  (#eq? @boolean "false"))

; Comments

[ (comment) (line_continuation) ] @comment

; Errors

(ERROR) @error
)TSQ";

// Scheme (.scm/.ss), tree-sitter-scheme: the grammar's own queries/highlights.scm.
static const char *kHighlightsScheme = R"TSQ(
["(" ")" "[" "]" "{" "}"] @punctuation.bracket

(number) @number
(character) @constant.builtin
(boolean) @constant.builtin
(symbol) @variable

(string) @string

(escape_sequence) @escape

(list
  .
  (symbol) @function)

(list
  .
  "["
  .
  (symbol)+ @variable
  .
  "]")

((symbol) @operator
 (#match? @operator "^(\\+|-|\\*|/|=|>|<|>=|<=)$"))

(list
  .
  (symbol) @keyword
  (#match? @keyword
   "^(define-syntax|let\\*|lambda|λ|case|=>|quote-splicing|unquote-splicing|set!|let|letrec|letrec-syntax|let-values|let\\*-values|do|else|define|cond|syntax-rules|unquote|begin|quote|let-syntax|and|if|quasiquote|letrec|delay|or|when|unless|identifier-syntax|assert|library|export|import|rename|only|except|prefix)$"
   ))

(list
  .
  (symbol) @function.builtin
  (#match? @function.builtin
   "^(caar|cadr|call-with-input-file|call-with-output-file|cdar|cddr|list|open-input-file|open-output-file|with-input-from-file|with-output-to-file|\\*|\\+|-|/|<|<=|=|>|>=|abs|acos|angle|append|apply|asin|assoc|assq|assv|atan|boolean\\?|caaaar|caaadr|caaar|caadar|caaddr|caadr|cadaar|cadadr|cadar|caddar|cadddr|caddr|call-with-current-continuation|call-with-values|car|cdaaar|cdaadr|cdaar|cdadar|cdaddr|cdadr|cddaar|cddadr|cddar|cdddar|cddddr|cdddr|cdr|ceiling|char->integer|char-alphabetic\\?|char-ci<=\\?|char-ci<\\?|char-ci=\\?|char-ci>=\\?|char-ci>\\?|char-downcase|char-lower-case\\?|char-numeric\\?|char-ready\\?|char-upcase|char-upper-case\\?|char-whitespace\\?|char<=\\?|char<\\?|char=\\?|char>=\\?|char>\\?|char\\?|close-input-port|close-output-port|complex\\?|cons|cos|current-error-port|current-input-port|current-output-port|denominator|display|dynamic-wind|eof-object\\?|eq\\?|equal\\?|eqv\\?|eval|even\\?|exact->inexact|exact\\?|exp|expt|floor|flush-output|for-each|force|gcd|imag-part|inexact->exact|inexact\\?|input-port\\?|integer->char|integer\\?|interaction-environment|lcm|length|list->string|list->vector|list-ref|list-tail|list\\?|load|log|magnitude|make-polar|make-rectangular|make-string|make-vector|map|max|member|memq|memv|min|modulo|negative\\?|newline|not|null-environment|null\\?|number->string|number\\?|numerator|odd\\?|output-port\\?|pair\\?|peek-char|positive\\?|procedure\\?|quotient|rational\\?|rationalize|read|read-char|real-part|real\\?|remainder|reverse|round|scheme-report-environment|set-car!|set-cdr!|sin|sqrt|string|string->list|string->number|string->symbol|string-append|string-ci<=\\?|string-ci<\\?|string-ci=\\?|string-ci>=\\?|string-ci>\\?|string-copy|string-fill!|string-length|string-ref|string-set!|string<=\\?|string<\\?|string=\\?|string>=\\?|string>\\?|string\\?|substring|symbol->string|symbol\\?|tan|transcript-off|transcript-on|truncate|values|vector|vector->list|vector-fill!|vector-length|vector-ref|vector-set!|vector\\?|write|write-char|zero\\?)$"
   ))

;; quote ;;

;; hardcoded highlight four levels of nested structure

; 'atom
(quote
  _ @constant)

; '()
(quote
  (_ _* @constant))

; '(())
(quote
  (_ (_ _* @constant)))

; '((()))
(quote
  (_ (_ (_ _* @constant))))

;; sexp comment ;;

;; hardcoded highlight four levels of nested structure

; #;atom
(comment
  _ @comment)

; #;(list)
(comment
  (_ _* @comment))

; #;(list (list))
(comment
  (_ (_ _* @comment)))

; #;(list (list (list)))
(comment
  (_ (_ (_ _ @comment))))

[(comment)
 (block_comment)
 (directive)] @comment
)TSQ";

// Tcl (.tcl), tree-sitter-tcl: the grammar's own queries/highlights.scm.
static const char *kHighlightsTcl = R"TSQ(
(comment) @comment

(command name: (simple_word) @function)

"proc" @keyword.function @keyword

(procedure
  name: (_) @variable
)

(set (id) @variable)

(argument
  name: (_) @variable.parameter @variable
)

((simple_word) @variable.builtin @variable
               (#any-of? @variable.builtin
                "argc"
                "argv"
                "argv0"
                "auto_path"
                "env"
                "errorCode"
                "errorInfo"
                "tcl_interactive"
                "tcl_library"
                "tcl_nonwordchars"
                "tcl_patchLevel"
                "tcl_pkgPath"
                "tcl_platform"
                "tcl_precision"
                "tcl_rcFileName"
                "tcl_traceCompile"
                "tcl_traceExec"
                "tcl_wordchars"
                "tcl_version"))


"expr" @function.builtin @function

; Highlight switch arguments as string
(command
    name: (simple_word) @keyword
    arguments:
        (word_list
            (braced_word
                (command
                    name: (simple_word) @string)))
    (#eq? @keyword "switch"))

(command
  name: (simple_word) @function.builtin @function
  (#any-of? @function.builtin
   "cd"
   "exec"
   "exit"
   "incr"
   "info"
   "join"
   "puts"
   "regexp"
   "regsub"
   "split"
   "subst"
   "trace"
   "source"))

; Highlight unset and variable arguments as variables
(command
    name: (simple_word) @keyword
    arguments: (word_list) @variable
    (#any-of? @keyword
        "unset"
        "variable"))

(command name: (simple_word) @keyword
         (#any-of? @keyword
          "append"
          "break"
          "catch"
          "continue"
          "default"
          "dict"
          "error"
          "eval"
          "global"
          "lappend"
          "lassign"
          "lindex"
          "linsert"
          "list"
          "llength"
          "lmap"
          "lrange"
          "lrepeat"
          "lreplace"
          "lreverse"
          "lsearch"
          "lset"
          "lsort"
          "package"
          "return"
          "trap"
          "throw"))

[
 "catch"
 "error"
 "global"
 "namespace"
 "on"
 "set"
 "try"
 "finally"
 ] @keyword

(unpack) @operator

[
 "while"
 "foreach"
 ; "for"
 ] @repeat @keyword

[
 "if"
 "else"
 "elseif"
 ] @conditional @keyword

[
 "**"
 "/" "*" "%" "+" "-"
 "<<" ">>"
 ">" "<" ">=" "<="
 "==" "!="
 "eq" "ne"
 "in" "ni"
 "&"
 "^"
 "|"
 "&&"
 "||"
 ] @operator

(variable_substitution) @variable
(quoted_word) @string
(escaped_character) @string.escape

[
 "{" "}"
 "[" "]"
 ";"
 ] @punctuation.bracket @punctuation.delimiter

(number) @number

((simple_word) @number
               (#match? @number
                   "^[0-9]+$|^[+-]?[0-9]+$"))


((simple_word) @boolean
               (#any-of? @boolean "true" "false"))

; after apply array auto_execok auto_import auto_load auto_mkindex auto_qualify
; auto_reset bgerror binary chan clock close coroutine dde encoding eof fblocked
; fconfigure fcopy file fileevent filename flush format gets glob history http
; interp load mathfunc mathop memory msgcat my next nextto open parray pid
; pkg::create pkg_mkIndex platform platform::shell pwd re_syntax read refchan
; registry rename safe scan seek self socket source string tailcall tcl::prefix
; tcl_endOfWord tcl_findLibrary tcl_startOfNextWord tcl_startOfPreviousWord
; tcl_wordBreakAfter tcl_wordBreakBefore tcltest tell time timerate tm
; transchan unknown unload update uplevel upvar vwait yield yieldto zlib
)TSQ";

// SQL (.sql), tree-sitter-sql 0.3.11 (DerekStride's): the grammar's own
// queries/highlights.scm.
static const char *kHighlightsSql = R"TSQ(
(object_reference
  name: (identifier) @type)

(invocation
  (object_reference
    name: (identifier) @function.call))

[
  (keyword_gist)
  (keyword_btree)
  (keyword_hash)
  (keyword_spgist)
  (keyword_gin)
  (keyword_brin)
  (keyword_array)
  (keyword_object_id)
] @function.call

(relation
  alias: (identifier) @variable)

(field
  name: (identifier) @field)

(term
  alias: (identifier) @variable)

((term
   value: (cast
    name: (keyword_cast) @function.call
    parameter: [(literal)]?)))

(literal) @string
(comment) @comment
(marginalia) @comment

((literal) @number
   (#match? @number "^[-+]?%d+$"))

((literal) @float
  (#match? @float "^[-+]?%d*\.%d*$"))

(parameter) @parameter

[
 (keyword_true)
 (keyword_false)
] @boolean

[
 (keyword_asc)
 (keyword_desc)
 (keyword_terminated)
 (keyword_escaped)
 (keyword_unsigned)
 (keyword_nulls)
 (keyword_last)
 (keyword_delimited)
 (keyword_replication)
 (keyword_auto_increment)
 (keyword_default)
 (keyword_collate)
 (keyword_concurrently)
 (keyword_engine)
 (keyword_always)
 (keyword_generated)
 (keyword_preceding)
 (keyword_following)
 (keyword_first)
 (keyword_current_timestamp)
 (keyword_immutable)
 (keyword_atomic)
 (keyword_parallel)
 (keyword_leakproof)
 (keyword_safe)
 (keyword_cost)
 (keyword_strict)
] @attribute

[
 (keyword_materialized)
 (keyword_recursive)
 (keyword_temp)
 (keyword_temporary)
 (keyword_unlogged)
 (keyword_external)
 (keyword_parquet)
 (keyword_csv)
 (keyword_rcfile)
 (keyword_textfile)
 (keyword_orc)
 (keyword_avro)
 (keyword_jsonfile)
 (keyword_sequencefile)
 (keyword_volatile)
] @storageclass

[
 (keyword_case)
 (keyword_when)
 (keyword_then)
 (keyword_else)
] @conditional

[
  (keyword_select)
  (keyword_from)
  (keyword_where)
  (keyword_index)
  (keyword_join)
  (keyword_primary)
  (keyword_delete)
  (keyword_create)
  (keyword_show)
  (keyword_unload)
  (keyword_insert)
  (keyword_merge)
  (keyword_distinct)
  (keyword_replace)
  (keyword_update)
  (keyword_into)
  (keyword_overwrite)
  (keyword_matched)
  (keyword_values)
  (keyword_value)
  (keyword_attribute)
  (keyword_set)
  (keyword_left)
  (keyword_right)
  (keyword_outer)
  (keyword_inner)
  (keyword_full)
  (keyword_order)
  (keyword_partition)
  (keyword_group)
  (keyword_with)
  (keyword_without)
  (keyword_as)
  (keyword_having)
  (keyword_limit)
  (keyword_offset)
  (keyword_table)
  (keyword_tables)
  (keyword_key)
  (keyword_references)
  (keyword_foreign)
  (keyword_constraint)
  (keyword_force)
  (keyword_use)
  (keyword_for)
  (keyword_if)
  (keyword_exists)
  (keyword_column)
  (keyword_columns)
  (keyword_cross)
  (keyword_lateral)
  (keyword_natural)
  (keyword_alter)
  (keyword_drop)
  (keyword_add)
  (keyword_view)
  (keyword_end)
  (keyword_is)
  (keyword_using)
  (keyword_between)
  (keyword_window)
  (keyword_no)
  (keyword_data)
  (keyword_type)
  (keyword_rename)
  (keyword_to)
  (keyword_schema)
  (keyword_owner)
  (keyword_authorization)
  (keyword_all)
  (keyword_any)
  (keyword_some)
  (keyword_returning)
  (keyword_begin)
  (keyword_commit)
  (keyword_rollback)
  (keyword_transaction)
  (keyword_only)
  (keyword_like)
  (keyword_similar)
  (keyword_over)
  (keyword_change)
  (keyword_modify)
  (keyword_after)
  (keyword_before)
  (keyword_range)
  (keyword_rows)
  (keyword_groups)
  (keyword_exclude)
  (keyword_current)
  (keyword_ties)
  (keyword_others)
  (keyword_zerofill)
  (keyword_format)
  (keyword_fields)
  (keyword_row)
  (keyword_sort)
  (keyword_compute)
  (keyword_comment)
  (keyword_location)
  (keyword_cached)
  (keyword_uncached)
  (keyword_lines)
  (keyword_stored)
  (keyword_virtual)
  (keyword_partitioned)
  (keyword_analyze)
  (keyword_explain)
  (keyword_verbose)
  (keyword_truncate)
  (keyword_rewrite)
  (keyword_optimize)
  (keyword_vacuum)
  (keyword_cache)
  (keyword_language)
  (keyword_called)
  (keyword_conflict)
  (keyword_declare)
  (keyword_filter)
  (keyword_function)
  (keyword_input)
  (keyword_name)
  (keyword_oid)
  (keyword_oids)
  (keyword_precision)
  (keyword_regclass)
  (keyword_regnamespace)
  (keyword_regproc)
  (keyword_regtype)
  (keyword_restricted)
  (keyword_return)
  (keyword_returns)
  (keyword_separator)
  (keyword_setof)
  (keyword_stable)
  (keyword_support)
  (keyword_tblproperties)
  (keyword_trigger)
  (keyword_unsafe)
  (keyword_admin)
  (keyword_connection)
  (keyword_cycle)
  (keyword_database)
  (keyword_encrypted)
  (keyword_increment)
  (keyword_logged)
  (keyword_none)
  (keyword_owned)
  (keyword_password)
  (keyword_reset)
  (keyword_role)
  (keyword_sequence)
  (keyword_start)
  (keyword_restart)
  (keyword_tablespace)
  (keyword_until)
  (keyword_user)
  (keyword_valid)
  (keyword_action)
  (keyword_definer)
  (keyword_invoker)
  (keyword_security)
  (keyword_extension)
  (keyword_version)
  (keyword_out)
  (keyword_inout)
  (keyword_variadic)
  (keyword_ordinality)
  (keyword_session)
  (keyword_isolation)
  (keyword_level)
  (keyword_serializable)
  (keyword_repeatable)
  (keyword_read)
  (keyword_write)
  (keyword_committed)
  (keyword_uncommitted)
  (keyword_deferrable)
  (keyword_names)
  (keyword_zone)
  (keyword_immediate)
  (keyword_deferred)
  (keyword_constraints)
  (keyword_snapshot)
  (keyword_characteristics)
  (keyword_off)
  (keyword_follows)
  (keyword_precedes)
  (keyword_each)
  (keyword_instead)
  (keyword_of)
  (keyword_initially)
  (keyword_old)
  (keyword_new)
  (keyword_referencing)
  (keyword_statement)
  (keyword_execute)
  (keyword_procedure)
  (keyword_copy)
  (keyword_delimiter)
  (keyword_encoding)
  (keyword_escape)
  (keyword_force_not_null)
  (keyword_force_null)
  (keyword_force_quote)
  (keyword_freeze)
  (keyword_header)
  (keyword_match)
  (keyword_program)
  (keyword_quote)
  (keyword_stdin)
  (keyword_extended)
  (keyword_main)
  (keyword_plain)
  (keyword_storage)
  (keyword_compression)
  (keyword_duplicate)
] @keyword

[
 (keyword_restrict)
 (keyword_unbounded)
 (keyword_unique)
 (keyword_cascade)
 (keyword_delayed)
 (keyword_high_priority)
 (keyword_low_priority)
 (keyword_ignore)
 (keyword_nothing)
 (keyword_check)
 (keyword_option)
 (keyword_local)
 (keyword_cascaded)
 (keyword_wait)
 (keyword_nowait)
 (keyword_metadata)
 (keyword_incremental)
 (keyword_bin_pack)
 (keyword_noscan)
 (keyword_stats)
 (keyword_statistics)
 (keyword_maxvalue)
 (keyword_minvalue)
] @type.qualifier

[
  (keyword_int)
  (keyword_null)
  (keyword_boolean)
  (keyword_binary)
  (keyword_varbinary)
  (keyword_image)
  (keyword_bit)
  (keyword_inet)
  (keyword_character)
  (keyword_smallserial)
  (keyword_serial)
  (keyword_bigserial)
  (keyword_smallint)
  (keyword_mediumint)
  (keyword_bigint)
  (keyword_tinyint)
  (keyword_decimal)
  (keyword_float)
  (keyword_double)
  (keyword_numeric)
  (keyword_real)
  (double)
  (keyword_money)
  (keyword_smallmoney)
  (keyword_char)
  (keyword_nchar)
  (keyword_varchar)
  (keyword_nvarchar)
  (keyword_varying)
  (keyword_text)
  (keyword_string)
  (keyword_uuid)
  (keyword_json)
  (keyword_jsonb)
  (keyword_xml)
  (keyword_bytea)
  (keyword_enum)
  (keyword_date)
  (keyword_datetime)
  (keyword_time)
  (keyword_datetime2)
  (keyword_datetimeoffset)
  (keyword_smalldatetime)
  (keyword_timestamp)
  (keyword_timestamptz)
  (keyword_geometry)
  (keyword_geography)
  (keyword_box2d)
  (keyword_box3d)
  (keyword_interval)
] @type.builtin

[
  (keyword_in)
  (keyword_and)
  (keyword_or)
  (keyword_not)
  (keyword_by)
  (keyword_on)
  (keyword_do)
  (keyword_union)
  (keyword_except)
  (keyword_intersect)
] @keyword.operator

[
  "+"
  "-"
  "*"
  "/"
  "%"
  "^"
  ":="
  "="
  "<"
  "<="
  "!="
  ">="
  ">"
  "<>"
  (op_other)
  (op_unary_other)
] @operator

[
  "("
  ")"
] @punctuation.bracket

[
  ";"
  ","
  "."
] @punctuation.delimiter
)TSQ";

// GAP (.g/.gi/.gd), gap-system/tree-sitter-gap (MIT): the grammar's own
// queries/highlights.scm. Its newer nvim capture names (keyword.conditional,
// number.float, ...) resolve through ts_apply_captures' first-segment fallback.
static const char *kHighlightsGap = R"TSQ(
; NOTE: (reiniscirpons) in case multiple queries match, last query wins. So
; queries should go from least specific to most specific. (This is the default
; behaviour since tree-sitter 0.22.2)
(identifier) @variable

; Constants
; convention: constants are of the form ALL_CAPS_AND_UNDERSCORES and have length at least 2
((identifier) @constant
  (#match? @constant "^[A-Z_][A-Z_]+$"))

; Functions
(assignment_statement
  left: (identifier) @function
  right: (function))

(assignment_statement
  left: (identifier) @function
  right: (atomic_function))

(assignment_statement
  left: (identifier) @function
  right: (lambda))

(call
  function: (identifier) @function.call)

((call
  function: (identifier) @function.builtin)
  (#any-of? @function.builtin "Assert" "Info" "IsBound" "Unbind" "TryNextMethod"))

(parameters
  (identifier) @variable.parameter)

(qualified_parameters
  (identifier) @variable.parameter)

(qualified_parameters
  (qualified_identifier
    (identifier) @variable.parameter))

(lambda_parameters
  (identifier) @variable.parameter)

; arg is treated specially when it is the only parameter of a function
((parameters
  .
  (identifier) @variable.parameter.builtin .)
  (#eq? @variable.parameter.builtin "arg"))

((qualified_parameters
  .
  (identifier) @variable.parameter.builtin .)
  (#eq? @variable.parameter.builtin "arg"))

((qualified_parameters
  .
  (qualified_identifier
    (identifier) @variable.parameter.builtin) .)
  (#eq? @variable.parameter.builtin "arg"))

((lambda_parameters
  .
  (identifier) @variable.parameter.builtin .)
  (#eq? @variable.parameter.builtin "arg"))

(locals
  (identifier) @variable.parameter)

; Literals
(bool) @constant.builtin

(integer) @number

(float) @number.float

(string) @string

(char) @character

(escape_sequence) @string.escape

[
  (help_topic)
  (help_book)
] @string.special

(tilde) @variable.builtin

; Record selectors
(record_entry
  left: [
    (identifier)
    (integer)
  ] @variable.member)

(record_selector
  selector: [
    (identifier)
    (integer)
  ] @variable.member)

(component_selector
  selector: [
    (identifier)
    (integer)
  ] @variable.member)

(function_call_option
  [
    (identifier)
    (record_entry ;Record entries specify global properties in function calls
      left: [
        (identifier)
        (integer)
      ])
  ] @property)

(help_statement
  (help_selector) @property)

; Operators
[
  "+"
  "-"
  "*"
  "/"
  "^"
  "->"
  ":="
  "<"
  "<="
  "<>"
  "="
  ">"
  ">="
  ".."
  (ellipsis)
] @operator

(help_statement
  (help_operator) @operator)

; Keywords
[
  (break_statement)
  (continue_statement)
  "atomic"
  (quit_statement)
] @keyword

[
  "function"
  "local"
  "end"
] @keyword.function

[
  "and"
  "in"
  "mod"
  "not"
  "or"
] @keyword.operator

"rec" @keyword.type

[
  "readonly"
  "readwrite"
] @keyword.modifier

(atomic_function
  "atomic" @keyword.modifier)

[
  "for"
  "while"
  "do"
  "od"
  "repeat"
  "until"
] @keyword.repeat

[
  "if"
  "then"
  "elif"
  "else"
  "fi"
] @keyword.conditional

"return" @keyword.return

(pragma) @keyword.directive

;Punctuation
[
  ","
  ";"
  "."
  "!."
  ":"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "!["
  "]"
  "{"
  "}"
] @punctuation.bracket

(help_statement
  "?" @punctuation.special)

;Comments
(comment) @comment
)TSQ";

// Maxima (.mac/.max), stewsat/tree-sitter-maxima -- see the query's own
// header for why it is mep's own rather than the grammar's.
static const char *kHighlightsMaxima = R"TSQ(
; Maxima (stewsat/tree-sitter-maxima). Written for mep: the grammar's own
; query ships without a license, so none of it is copied here. The grammar
; has no field names -- a call's head is simply its first atom.

(identifier) @variable

(binary_expression . (atom (identifier) @variable) ":")

(function_call . (atom (identifier) @function.call))

((function_call . (atom (identifier) @function.builtin))
 (#any-of? @function.builtin
  "print" "display" "disp" "ldisp" "printf" "makelist" "map" "apply" "lambda"
  "expand" "factor" "ratsimp" "fullratsimp" "trigsimp" "simplify" "subst" "ev"
  "diff" "integrate" "limit" "sum" "product" "taylor" "solve" "linsolve"
  "allroots" "realroots" "sqrt" "abs" "exp" "log" "sin" "cos" "tan" "asin"
  "acos" "atan" "sinh" "cosh" "tanh" "float" "bfloat" "length" "first" "last"
  "rest" "append" "cons" "reverse" "sort" "matrix" "determinant" "invert"
  "transpose" "load" "kill" "plot2d" "plot3d" "return" "error" "is" "mod"))

; f(x) := ..., f(x) ::= ...
(binary_expression
  . (function_call . (atom (identifier) @function))
  [":=" "::="])

(lambda_expression (list (atom (identifier) @variable.parameter)))
(block_expression . (list (atom (identifier) @variable.parameter)))

((identifier) @constant.builtin
 (#any-of? @constant.builtin
  "%pi" "%e" "%i" "%phi" "%gamma" "inf" "minf" "infinity" "und" "ind" "%o"))

((identifier) @boolean (#any-of? @boolean "true" "false"))

["if" "then" "else" "elseif" "unless"] @conditional
["for" "while" "do" "thru" "step" "next" "in"] @repeat
["lambda" "block"] @keyword.function
["and" "or" "not"] @keyword.operator

[":" "::" ":=" "::=" "+" "-" "*" "/" "^" "^^" "." "=" "<" "<=" ">" ">="
 "!" "!!" "#" "'" "''" "@"] @operator

[";" "$" ","] @punctuation.delimiter
["(" ")" "[" "]" "{" "}"] @punctuation.bracket

(number) @number
(string) @string
(comment) @comment
)TSQ";

static const char *kHighlightsCrystal = R"TSQ(
; Early rules
; These patterns may be overridden later

[
  ","
  ";"
  "."
  ":"
] @punctuation.delimiter

; Keywords

[
  "alias"
  "annotation"
  "asm"
  "begin"
  "break"
  "case"
  "do"
  "end"
  "ensure"
  "extend"
  "in"
  "include"
  "next"
  "of"
  "select"
  "then"
  "verbatim"
  "when"
] @keyword

[
  "def"
  "fun"
  "macro"
] @keyword.function

[
  "class"
  "enum"
  "lib"
  "module"
  "struct"
  "type"
  "union"
] @keyword.type

"require" @keyword.import

[
  "return"
  "yield"
] @keyword.return

[
  "if"
  "else"
  "elsif"
  "unless"
] @keyword.conditional

(conditional
  [
    "?"
    ":"
  ] @keyword.conditional.ternary)

[
  "for"
  "until"
  "while"
] @keyword.repeat

"rescue" @keyword.exception

[
  (private)
  (protected)
  "abstract"
] @keyword.modifier

(pseudo_constant) @constant.builtin

; literals
(string
  "\"" @string)

(string
  (literal_content) @string)

(string
  (escape_sequence) @string.escape)

(symbol
  [
    ":"
    ":\""
    "\""
  ] @string.special.symbol)

(symbol
  (literal_content) @string.special.symbol)

(symbol
  (escape_sequence) @character)

(command
  "`" @string.special)

(command
  (literal_content) @string.special)

(command
  (escape_sequence) @character)

(regex
  "/" @punctuation.bracket)

(regex
  (literal_content) @string.regexp)

(regex_modifier) @character.special

(heredoc_body
  (literal_content) @string)

(heredoc_body
  (escape_sequence) @string.escape)

[
  (heredoc_start)
  (heredoc_end)
] @label

(char
  "'" @character)

(char
  (literal_content) @character)

(char
  (escape_sequence) @string.escape)

(integer) @number

(float) @number.float

[
  (true)
  (false)
] @boolean

(nil) @constant.builtin

((comment) @comment
  ; Set priority so macro expressions in comments are not grayed out
  (#set! priority 95))

; Operators and punctuation
[
  "="
  "=>"
  "->"
  "&"
  "*"
  "**"
  (operator)
] @operator

[
  "("
  ")"
  "["
  "@["
  "]"
  "{"
  "}"
] @punctuation.bracket

([
  "{{"
  "}}"
] @punctuation.bracket
  ; Set priority so "a{{b}}" is highlighted as brackets, not string content
  ;                   ^^
  (#set! priority 105))

(index_call
  method: (operator) @punctuation.bracket
  [
    "]"
    "]?"
  ] @punctuation.bracket)

(block
  "|" @punctuation.bracket)

[
  "{%"
  "%}"
] @tag.delimiter

(interpolation
  "#{" @punctuation.special
  "}" @punctuation.special)

; Types
[
  (constant)
  (generic_instance_type)
  (generic_type)
] @type

(nilable_constant
  "?" @type.builtin)

(nilable_type
  "?" @type.builtin)

(union_type
  "|" @operator)

(annotation
  (constant) @attribute)

(method_def
  name: (identifier) @function.method)

(macro_def
  name: (identifier) @function.method)

(abstract_method_def
  name: (identifier) @function.method)

(fun_def
  name: (identifier) @function
  real_name: (identifier)? @function)

(param
  name: (identifier) @variable.parameter)

(splat_param
  name: (identifier) @variable.parameter)

(double_splat_param
  name: (identifier) @variable.parameter)

(block_param
  name: (identifier) @variable.parameter)

(fun_param
  name: (identifier) @variable.parameter)

(rescue
  variable: (identifier) @variable.parameter)

(macro_var
  name: (identifier) @variable)

[
  (class_var)
  (instance_var)
] @variable.member

(underscore) @variable.parameter.builtin

(self) @variable.builtin

(named_tuple
  (named_expr
    name: (identifier) @property))

(argument_list
  (named_expr
    name: (identifier) @property))

(named_type
  name: (identifier) @property)

; function calls
(call
  method: (identifier) @function.call)
)TSQ";

// LaTeX (latex-lsp/tree-sitter-latex, a dynamically-loaded grammar like
// every other DynamicLanguageTable entry -- see flake.nix's tsGrammars
// list). Unlike most grammars, upstream ships no highlights.scm of its
// own, so this query is written from scratch against the grammar's
// node-types.json (verified against the nixpkgs-pinned 0.6.0 build by
// compiling a real TSQuery, same as every other query in this file) --
// with nvim-treesitter's community query as a structural reference, but
// captures renamed to ones mep.ts_capture_hl (kBuiltinSyntax, main.cpp)
// actually resolves: sectioning titles reuse markdown's own
// text.title.1..6 per-depth progression so a \section heading colors
// like the equivalent `##` markdown heading; math zones use text.math
// (added to ts_capture_hl alongside this query). Span ordering does the
// rest: TreesitterHighlight paints widest-first, so a whole
// displayed_equation's text.math wash goes down first and narrower
// captures inside it (command_name, delimiters) win over it.
static const char *kHighlightsLatex = R"TSQ(
; Comments
[
  (line_comment)
  (block_comment)
  (comment_environment)
] @comment

; Every \command gets the function color as the baseline; more specific
; patterns below (sectioning, includes, begin/end) paint over it. TeX
; conditionals are excluded here rather than just recaptured by the
; @keyword patterns at the bottom: both captures would cover the exact
; same span, and TreesitterHighlight's widest-first paint order breaks
; equal-length ties arbitrarily -- excluding them from the baseline is
; what makes the keyword color deterministic.
((command_name) @function
  (#not-match? @function "^\\\\(if[a-zA-Z@]*|fi|else)$"))

; \begin{env} / \end{env}
(begin
  command: _ @keyword
  name: (curly_group_text
    (text) @type))
(end
  command: _ @keyword
  name: (curly_group_text
    (text) @type))

; Sectioning: same hottest-to-coolest-by-depth text.title.N progression
; markdown headings use, so the sidebar/structure colors match the buffer.
(part
  command: _ @keyword
  text: (curly_group
    (_) @text.title.1))
(chapter
  command: _ @keyword
  text: (curly_group
    (_) @text.title.1))
(section
  command: _ @keyword
  text: (curly_group
    (_) @text.title.2))
(subsection
  command: _ @keyword
  text: (curly_group
    (_) @text.title.3))
(subsubsection
  command: _ @keyword
  text: (curly_group
    (_) @text.title.4))
(paragraph
  command: _ @keyword
  text: (curly_group
    (_) @text.title.5))
(subparagraph
  command: _ @keyword
  text: (curly_group
    (_) @text.title.6))
(title_declaration
  command: _ @keyword
  text: (curly_group
    (_) @text.title.1))
(author_declaration
  command: _ @keyword)

; Math zones: a flat wash over the whole formula; commands/delimiters
; inside repaint themselves via narrower captures.
[
  (displayed_equation)
  (inline_formula)
] @text.math
(math_environment) @text.math

; Preamble / file inclusion
(class_include
  command: _ @include
  path: (curly_group_path) @string)
(package_include
  command: _ @include
  paths: (curly_group_path_list) @string)
(latex_include
  command: _ @include
  path: (curly_group_path) @string)
(bibtex_include
  command: _ @include
  paths: (curly_group_path_list) @string)
(biblatex_include
  glob: (curly_group_glob_pattern) @string)
(graphics_include
  command: _ @include
  path: (curly_group_path) @string)

; Labels, references, citations
(label_definition
  command: _ @function
  name: (curly_group_label
    (_) @text.reference))
(label_reference
  command: _ @function
  names: (curly_group_label_list
    (_) @text.reference))
(citation
  command: _ @function
  keys: (curly_group_text_list
    (_) @text.reference))
(hyperlink
  command: _ @function
  uri: (curly_group_uri
    (_) @text.uri))

; Command/environment definitions
(new_command_definition
  command: _ @function.macro)
(old_command_definition
  command: _ @function.macro)
(environment_definition
  command: _ @function.macro
  name: (curly_group_text
    (_) @type))

; Key-value options ([width=0.5\textwidth, ...])
(key_value_pair
  key: (_) @attribute)

; Inline emphasis, mirroring markdown's text.strong/text.emphasis colors
((generic_command
  command: (command_name) @_cmd
  arg: (curly_group
    (_) @text.emphasis))
  (#any-of? @_cmd "\\emph" "\\textit" "\\itshape"))
((generic_command
  command: (command_name) @_cmd
  arg: (curly_group
    (_) @text.strong))
  (#any-of? @_cmd "\\textbf" "\\bfseries"))

; Verbatim bodies render literal-green, like markdown code spans
(verbatim_environment) @text.literal

; TeX conditionals (\ifdefined ... \else ... \fi)
((generic_command
  command: (command_name) @keyword)
  (#match? @keyword "^\\\\if[a-zA-Z@]*$"))
((generic_command
  command: (command_name) @keyword)
  (#any-of? @keyword "\\fi" "\\else"))

; \todo{...} and friends stand out as warnings
(todo
  command: _ @warn)
)TSQ";

// BibTeX (latex-lsp/tree-sitter-bibtex, dynamically loaded like the
// latex grammar above -- .bib files sit alongside .tex ones in any real
// LaTeX project, so the two ship together). Adapted from upstream's own
// queries/highlights.scm with captures renamed to mep.ts_capture_hl's
// vocabulary: entry keys use text.reference so a key colors the same in
// the .bib file as its \cite{key} usage does in the .tex one.
static const char *kHighlightsBibtex = R"TSQ(
[
  (string_type)
  (preamble_type)
  (entry_type)
] @keyword

[
  (junk)
  (comment)
] @comment

(command) @function

(number) @number

(field
  name: (identifier) @attribute)

[
  (brace_word)
  (quote_word)
] @string

[
  (key_brace)
  (key_paren)
] @text.reference

(string
  name: (identifier) @constant)
)TSQ";

// Fold queries (Phase 19's own noted gap -- "No fold-query support",
// see main.cpp's kBuiltinSyntax comment): one `@fold` capture per
// syntactic block worth collapsing, for the core compiled-in languages
// that have a grammar with distinct block/body node types (markdown
// still has its own line-based heading-depth fold provider in main.cpp's
// kBuiltinMarkdown -- a second, tree-sitter-driven fold provider for the
// same regions would just double up on markdown's existing folds under a
// different provider name). Every node name below was verified against
// the actual vendored grammar (third_party/grammars/<lang>) by compiling
// a real TSQuery against it -- see the incremental-reparse work in
// treesitter.cpp for how these are executed (TreesitterFoldRanges).
//
// Org is the one exception that *does* get a real query here (kFoldsOrg,
// below) despite also having its own line-based provider (main.cpp's
// kBuiltinOrg, `mep.org_fold_all`): that provider's headline scan --
// and Editor::RecomputeOrgFolds' own C++ one (editor.cpp), which used
// the same approach -- just regex-matched every line against
// `^(%*+)%s+`, with no idea it might be looking at a `#+begin_src`
// block's contents rather than real document structure. Source code
// with a `* comment` or `char *p = ...` sitting at column 0 inside a
// code block got misparsed as a headline, corrupting the whole nesting
// stack for the rest of the file -- real headlines merging into far
// fewer, wrong folds. Org's actual grammar has a context-aware external
// scanner that already gets this right (a `block`'s `contents` parse as
// plain text, never as a nested `headline`), so `(section) @fold` -- one
// capture per headline's own node, an org `section` already spanning
// exactly that headline through its last nested subsection, body and
// all -- sidesteps the bug entirely instead of reimplementing block-
// awareness by hand.
static const char *kFoldsC = R"TSQ(
[
  (compound_statement)
  (struct_specifier)
  (enum_specifier)
  (initializer_list)
] @fold
)TSQ";

static const char *kFoldsCpp = R"TSQ(
[
  (compound_statement)
  (class_specifier)
  (struct_specifier)
  (namespace_definition)
  (field_declaration_list)
  (declaration_list)
  (enumerator_list)
  (initializer_list)
  (lambda_expression)
] @fold
)TSQ";

static const char *kFoldsLua = R"TSQ(
[
  (function_declaration)
  (function_definition)
  (if_statement)
  (for_statement)
  (while_statement)
  (repeat_statement)
  (do_statement)
  (table_constructor)
] @fold
)TSQ";

static const char *kFoldsPython = R"TSQ(
[
  (function_definition)
  (class_definition)
  (if_statement)
  (for_statement)
  (while_statement)
  (try_statement)
  (with_statement)
  (match_statement)
  (dictionary)
  (list)
] @fold
)TSQ";

// JS block-bodied constructs (if/for/while/try/catch/finally/functions)
// all share one `statement_block` node for their body, so that alone
// covers them uniformly without listing each statement kind separately.
static const char *kFoldsJavascript = R"TSQ(
[
  (function_declaration)
  (function_expression)
  (arrow_function)
  (generator_function)
  (generator_function_declaration)
  (class_body)
  (statement_block)
  (object)
  (array)
  (switch_body)
] @fold
)TSQ";

// One `section` per headline (recursive: a level-1 headline's `section`
// already contains its level-2/3/... children's own nested `section`
// nodes, body and all), so this alone reproduces the full headline-depth
// nesting mep's org fold providers computed by hand -- see the comment
// above this block for why org gets a real query here unlike every other
// filetype with its own line-based provider. `block` is org's own generic
// `#+begin_X ... #+end_X` node (kHighlightsOrg's `@OrgBlock` capture is the
// same node) -- covers `#+begin_src`, but also `#+begin_example`/`_quote`/
// `_verse`/etc. uniformly, matching real Emacs org-mode's own "every
// greater block is foldable" behavior rather than singling out src blocks
// with a name-matching predicate. Nests naturally inside a `section`'s own
// fold (a block's a few lines inside its enclosing headline's body) --
// FoldNestingDepth/MaxFoldNestingDepth (editor.cpp) already handle nested
// folds generically, so za/zm/zr and the gutter marker need no changes at
// all to pick these up alongside headline folds under the same "org"
// provider (RecomputeOrgFolds, editor.cpp).
static const char *kFoldsOrg = R"TSQ(
[
  (section)
  (block)
] @fold
)TSQ";

