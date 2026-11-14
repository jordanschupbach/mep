; Document outline for mep's structure sidebar: @definition.<kind> on the
; whole definition, @name on the node holding its name.

(tradfn header: (tradfn_header name: (identifier) @name)) @definition.function
(tradfn header: (tradfn_header (operator_spec operator: (identifier) @name))) @definition.function

; name←{...}: a dfn (or dop) assigned to a name.
(assignment target: (identifier) @name . "←" . value: (dfn) .) @definition.function

(namespace name: (_) @name) @definition.namespace
(class_definition name: (identifier) @name) @definition.class
