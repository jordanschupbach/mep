; Highlight captures for mep's Crystal grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after.

; ---- Names ------------------------------------------------------------------

(identifier) @variable

[
  (instance_var)
  (class_var)
] @variable.member

(macro_var) @variable

(special_variable) @variable.builtin

(self) @variable.builtin

(underscore) @variable.builtin

(constant) @type

(pseudo_constant) @constant.builtin

; A constant's definition and an enum's members are values. (A constant
; used elsewhere stays @type: `IO`, `JSON` and a generic's `T` are types
; though they look like ALL_CAPS values.)
(const_assign lhs: (constant) @constant)

(enum_def
  body: (expressions (constant) @constant))

; ---- Types ------------------------------------------------------------------

[
  (generic_type)
  (generic_instance_type)
  (nilable_constant)
] @type

(union_type "|" @operator)

(nilable_type "?" @type.builtin)
(nilable_constant "?" @type.builtin)
(class_type "class" @type.builtin)

; The primitive and core types.
((constant) @type.builtin
 (#any-of? @type.builtin
  "Bool" "Char" "Float32" "Float64" "Int8" "Int16" "Int32" "Int64" "Int128" "Nil" "NoReturn"
  "Object" "Pointer" "Proc" "Reference" "String" "Symbol" "UInt8" "UInt16" "UInt32" "UInt64"
  "UInt128" "Value" "Void"))

; ---- Definitions -------------------------------------------------------------

(module_def name: [(constant) @type.definition (generic_type (constant) @type.definition)])
(class_def name: [(constant) @type.definition (generic_type (constant) @type.definition)])
(struct_def name: [(constant) @type.definition (generic_type (constant) @type.definition)])
(enum_def name: (constant) @type.definition)
(lib_def name: [(constant) @module (generic_type (constant) @module)])
(annotation_def name: (constant) @attribute)
(c_struct_def name: (constant) @type.definition)
(union_def name: (constant) @type.definition)
(type_def name: (constant) @type.definition)
(alias name: (constant) @type.definition)

(method_def name: [(identifier) (operator)] @function.method)
(abstract_method_def name: [(identifier) (operator)] @function.method)
(macro_def name: [(identifier) (operator)] @function.macro)
(fun_def name: [(identifier) (constant)] @function)
(fun_def real_name: [(identifier) (constant)] @function)

(param name: (identifier) @variable.parameter)
(param extern_name: (identifier) @variable.parameter)
(splat_param name: (identifier) @variable.parameter)
(double_splat_param name: (identifier) @variable.parameter)
(block_param name: (identifier) @variable.parameter)
(fun_param name: (identifier) @variable.parameter)
(rescue variable: (identifier) @variable.parameter)
(forall (constant) @type)

; ---- Calls ------------------------------------------------------------------

(call method: (identifier) @function.call)
(call method: (constant) @function.call)
(implicit_object_call method: (identifier) @function.call)
(method_proc method: (identifier) @function.call)

; Methods every object has that read like keywords.
(call
  method: (identifier) @function.builtin
  (#any-of? @function.builtin
   "puts" "print" "p" "pp" "raise" "loop" "spawn" "sleep" "gets" "exit" "abort" "at_exit"
   "caller" "debugger" "getter" "setter" "property" "getter?" "property?" "getter!" "setter!"
   "property!" "class_getter" "class_setter" "class_property" "record" "def_equals"
   "def_hash" "def_equals_and_hash" "delegate" "forward_missing_to" "previous_def" "super"
   "instance_variable_get"))

; `x.as(T)`, `x.is_a?(T)`, `x.nil?`, `x.responds_to?(:m)`: the compiler's.
(call
  method: (identifier) @keyword.operator
  (#any-of? @keyword.operator "as" "as?" "is_a?" "nil?" "responds_to?"))

(named_expr name: (identifier) @property)
(named_type name: (identifier) @property)

; ---- Keywords ---------------------------------------------------------------

[
  "alias"
  "annotation"
  "asm"
  "begin"
  "do"
  "end"
  "extend"
  "forall"
  "include"
  "of"
  "out"
  "select"
  "then"
  "uninitialized"
  "verbatim"
  "with"
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

[
  "abstract"
  (private)
  (protected)
] @keyword.modifier

"require" @keyword.import

[
  "return"
  "next"
  "break"
  "yield"
] @keyword.return

[
  "if"
  "unless"
  "elsif"
  "else"
  "case"
  "when"
  "in"
] @keyword.conditional

[
  "while"
  "until"
  "for"
] @keyword.repeat

[
  "rescue"
  "ensure"
] @keyword.exception

[
  "typeof"
  "sizeof"
  "instance_sizeof"
  "alignof"
  "instance_alignof"
  "offsetof"
  "pointerof"
] @keyword.operator

(conditional ["?" ":"] @keyword.conditional.ternary)

; ---- Literals ---------------------------------------------------------------

(string) @string
(chained_string) @string
(command) @string.special
(heredoc_start) @label
(heredoc_end) @label
(heredoc_body (literal_content) @string)
(regex) @string.regexp
(regex_modifier) @character.special
(symbol) @string.special.symbol
(char) @character
(escape_sequence) @string.escape
(integer) @number
(float) @number.float
[(true) (false)] @boolean
(nil) @constant.builtin

; A symbol's or regex's own escapes stay in its colour's family.
(symbol (escape_sequence) @character)

(annotation "@[" @attribute "]" @attribute)
(annotation (constant) @attribute)

(comment) @comment
[
  (loc_pragma_push)
  (loc_pragma_pop)
  (loc_pragma_location)
] @keyword.directive

; ---- Operators and punctuation ----------------------------------------------

[
  "="
  "=>"
  "->"
  "&"
  "*"
  "**"
  "!"
  "~"
  (operator)
] @operator

[
  ","
  ";"
  "."
  "::"
] @punctuation.delimiter

; A pointer type's star is part of the type, not multiplication.
(pointer_type "*" @type.builtin)

[
  "("
  ")"
  "["
  "]"
  "]?"
  "{"
  "}"
] @punctuation.bracket

(block "|" @punctuation.bracket)

; %w(...) and %i(...) are arrays whose delimiters are the literal's own:
; string-coloured like the words, not brackets.
((array "[" @string) (#match? @string "^%"))
((array "]" @string) (#not-eq? @string "]"))

(index_call
  method: (operator) @punctuation.bracket)

; Interpolation: the delimiters stand out, what's inside is code.
(interpolation
  "#{" @punctuation.special
  "}" @punctuation.special)

; ---- Macros -----------------------------------------------------------------

[
  "{{"
  "}}"
  "{%"
  "%}"
] @punctuation.special

