#pragma once

// Highlight queries for the dynamically-loaded languages (treesitter.cpp's
// TS_DYNAMIC_LANGUAGES); the compiled-in ones' are their grammars' own,
// embedded via <name>_queries.h. Embedded verbatim from each grammar
// repo's queries/highlights.scm at
// the tag pinned in CMakeLists.txt (add_ts_grammar / the TS_GRAMMARS
// table), fetched at vendoring time -- not regenerated at build time, so
// mep's build has no extra network dependency. A handful of grammars
// ship no highlights.scm of their own (hcl, kotlin, verilog, graphql);
// those four come from nvim-treesitter's community query set instead
// (github.com/nvim-treesitter/nvim-treesitter, MIT licensed), matched
// against the exact grammar/node-type version it was written for.
// Predicates ((#match? ...), (#eq? ...), (#any-of? ...), and
// nvim-treesitter's own (#lua-match? ...) alias) are evaluated at
// runtime by EvalPredicates in treesitter.cpp; scope-only predicates
// ((#is-not? local) and friends) are accepted as-is -- see the comment
// there for the exact list this integration does and doesn't handle.

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

// Perl/D (and Fortran and Nim, since compiled in) (added for org-babel src-block syntax
// highlighting -- mep.org_babel_langs supports them but no grammar
// query existed for them yet; Crystal, added with them, is now mep's own
// grammar, grammars/tree-sitter-crystal). Fortran/D/Nim were vendored
// verbatim from each grammar's own upstream queries/highlights.scm, same
// convention as every other dynamically-loaded language above. Perl is a hand-written
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

// (Fortran is compiled in: grammars/tree-sitter-fortran/queries.)

// (D is compiled in: grammars/tree-sitter-d/queries.)


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
