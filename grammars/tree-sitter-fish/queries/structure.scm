; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `function name ...; end`. A function defined inside another nests under it.
(function_definition name: (_) @name) @definition.function

; Top-level `set` of a global or universal variable, or an exported one --
; the settings a config file exists for (`set -gx EDITOR mep`). Plain
; `set x 1` stays out of the outline.
(program
  (command
    name: (word) @_set
    argument: (word) @_scope
    .
    argument: (_) @name) @definition.variable
  (#eq? @_set "set")
  (#match? @_scope "^-[a-zA-Z]*[gUx][a-zA-Z]*$|^--(global|universal|export)$")
  (#not-match? @name "^-"))

; `abbr -a name ...` and `alias name ...`: commands a config file defines.
(program
  (command
    name: (word) @_abbr
    argument: (word) @_add
    .
    argument: (_) @name) @definition.function
  (#eq? @_abbr "abbr")
  (#match? @_add "^(-a|--add)$"))

(program
  (command
    name: (word) @_alias
    .
    argument: (_) @name) @definition.function
  (#eq? @_alias "alias"))
