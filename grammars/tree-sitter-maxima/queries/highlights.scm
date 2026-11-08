; Maxima (mep's own grammar, grammars/tree-sitter-maxima). The grammar has
; no field names -- a call's head is simply its first atom. General
; patterns first; later, more specific ones win.

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
(block_expression . (list (binary_expression . (atom (identifier) @variable.parameter) ":")))
(for_expression . (identifier) @variable.parameter)
(for_in_expression . (identifier) @variable.parameter)

; `?name`: a symbol of the host Lisp.
((identifier) @function.builtin (#match? @function.builtin "^\\?"))

((identifier) @constant.builtin
 (#any-of? @constant.builtin
  "%pi" "%e" "%i" "%phi" "%gamma" "inf" "minf" "infinity" "und" "ind" "%o"))

((identifier) @boolean (#any-of? @boolean "true" "false"))

; `tag && expr`
(binary_expression . (identifier) @label "&&")

["if" "then" "else" "elseif" "unless"] @conditional
["for" "while" "do" "thru" "step" "next" "from" "in"] @repeat
["lambda" "block"] @keyword.function
["and" "or" "not"] @keyword.operator

[":" "::" ":=" "::=" "+" "-" "*" "/" "^" "^^" "**" "." "=" "<" "<=" ">" ">="
 "#" "->" "!" "!!" "'" "''" "@" "&&"] @operator

[";" "$" ","] @punctuation.delimiter
["(" ")" "[" "]" "{" "}"] @punctuation.bracket

(number) @number
(string) @string
(comment) @comment
(lisp_escape) @string.special
