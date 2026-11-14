; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; Programs.
(program_definition
 (identification_division (program_name) @name)) @definition.module

; Procedure division sections and paragraphs.
(section_header name: (_) @name) @definition.namespace
(paragraph_header name: (_) @name) @definition.function

; Records: level 01 and 77 entries, and file descriptions.
(data_description
 (level_number) @_level
 (entry_name) @name
 (#match? @_level "^(0?1|77)$")) @definition.variable
(file_description (file_description_entry (WORD) @name)) @definition.struct
