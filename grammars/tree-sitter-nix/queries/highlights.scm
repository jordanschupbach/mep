; Highlight captures for mep's Nix grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.

; ---- names ----------------------------------------------------------------

(variable_expression name: (identifier) @variable)

(attrpath attr: (identifier) @property)
(inherited_attrs attr: (identifier) @property)

(function_expression universal: (identifier) @variable.parameter)
(formal name: (identifier) @variable.parameter)

; A called function: `f x`, and the last attr of `lib.mkIf c x`.
(apply_expression
  function: (variable_expression name: (identifier) @function.call))
(apply_expression
  function: (select_expression
    attrpath: (attrpath attr: (identifier) @function.call .)))

; The builtins Nix puts in global scope, called or not.
((variable_expression name: (identifier) @function.builtin)
 (#any-of? @function.builtin
  "abort" "baseNameOf" "break" "derivation" "derivationStrict" "dirOf"
  "fetchGit" "fetchMercurial" "fetchTarball" "fetchTree" "fromTOML" "import"
  "isNull" "map" "placeholder" "removeAttrs" "scopedImport" "throw"
  "toString"))

((variable_expression name: (identifier) @variable.builtin)
 (#any-of? @variable.builtin
  "builtins" "__currentSystem" "__currentTime" "__nixPath" "__nixVersion"
  "__storeDir" "__langVersion"))

; `builtins.foldl'`, `builtins.attrNames`, ...
(select_expression
  expression: (variable_expression name: (identifier) @_builtins)
  attrpath: (attrpath . attr: (identifier) @function.builtin)
  (#eq? @_builtins "builtins"))

((variable_expression name: (identifier) @boolean)
 (#any-of? @boolean "true" "false"))

((variable_expression name: (identifier) @constant.builtin)
 (#eq? @constant.builtin "null"))

; ---- keywords and operators -----------------------------------------------

[
  "assert"
  "with"
  "let"
  "in"
  "rec"
  "inherit"
  "or"
] @keyword

[
  "if"
  "then"
  "else"
] @conditional

(unary_expression operator: _ @operator)
(binary_expression operator: _ @operator)
(has_attr_expression operator: _ @operator)

[
  "="
  "@"
  "?"
] @operator

; ---- literals -------------------------------------------------------------

(integer_expression) @number
(float_expression) @float

; A string's quotes and text, not the whole node, so the code inside a
; `${...}` keeps its own colours.
(string_expression "\"" @string)
(indented_string_expression "''" @string)
(string_fragment) @string
(indented_string_fragment) @string
(escape_sequence) @string.escape

(interpolation
  "${" @punctuation.interpolation
  "}" @punctuation.interpolation)

(path_fragment) @string.special.path
(spath_expression) @string.special.path
(uri_expression) @string.special.uri

; ---- punctuation ----------------------------------------------------------

[
  ";"
  "."
  ","
  ":"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

(ellipses) @punctuation.special

(comment) @comment
