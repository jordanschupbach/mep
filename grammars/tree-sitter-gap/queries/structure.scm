; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `F := function(...) ... end;` (also atomic functions and `x -> ...`
; lambdas). A function assigned inside another nests under it.
(assignment_statement
  left: (identifier) @name
  right: [(function) (atomic_function) (lambda)]) @definition.function

; Top-level `X := value;` -- the globals a library file sets up.
(source_file
  (assignment_statement
    left: (identifier) @name
    right: (_) @_value) @definition.variable
  (#not-match? @_value "^(function|atomic|[A-Za-z_0-9@]+[ \t]*->)"))

; The declarations GAP's library is made of:
; DeclareOperation("Name", ...), DeclareGlobalFunction("Name"),
; BindGlobal("Name", ...), DeclareInfoClass("Name") ...
(call
  function: (identifier) @_decl
  (argument_list . (string (string_content) @name))
  (#match? @_decl "^(Declare[A-Za-z]*|BindGlobal|BindConstant|NewCategory|NewRepresentation|NewFilter|NewOperation|NewAttribute|NewProperty|NewInfoClass)$")) @definition.type

; InstallMethod(Name, "for ...", ...) and its relatives: implementations,
; nested under nothing (they are top-level calls), named by the operation.
(call
  function: (identifier) @_inst
  (argument_list . (identifier) @name)
  (#match? @_inst "^(Install[A-Za-z]*|InstallGlobalFunction|InstallValue|InstallMethod|InstallOtherMethod|InstallImmediateMethod|InstallTrueMethod)$")) @definition.method
