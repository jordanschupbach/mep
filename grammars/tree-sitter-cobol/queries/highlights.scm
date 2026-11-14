; COBOL highlight captures (nvim-treesitter naming). The grammar shows every
; reserved word as a `keyword` node; the rest are the grammar's own nodes.
; Later patterns win over earlier ones on the same text.

(keyword) @keyword

; Literals.
[
 (string)
 (x_string)
 (h_string)
 (n_string)
] @string
(pseudo_text) @string.special
(number) @number

; Figurative constants and conditions.
[
 (ZERO)
 (ZEROS)
 (SPACE)
 (QUOTE)
 (HIGH_VALUE)
 (LOW_VALUE)
 (TOK_NULL)
] @constant.builtin
[
 (TRUE)
 (FALSE)
] @boolean

; PICTURE strings.
[
 (picture_x)
 (picture_n)
 (picture_9)
 (picture_a)
 (picture_edit)
] @constant

; Data division: level numbers, and the names records (01, 77) and
; condition names / constants (88, 78) declare.
(level_number) @type.builtin
(data_description
 (level_number) @_level
 (entry_name) @constructor
 (#match? @_level "^(0?1|77)$"))
(data_description
 (level_number) @_level
 (entry_name) @constant
 (#match? @_level "^(88|78)$"))
(constant_entry name: (WORD) @constant)

; Program, section and paragraph names; procedures and functions called.
(program_name) @module
(section_header name: (_) @function)
(paragraph_header name: (_) @function)
(perform_procedure (label (qualified_word (WORD) @function.call)))
(goto_statement (label (qualified_word (WORD) @function.call)))
(function_ (WORD) @function.builtin)
[
 (CURRENT_DATE_FUNC)
 (WHEN_COMPILED_FUNC)
 (UPPER_CASE_FUNC)
 (LOWER_CASE_FUNC)
 (TRIM_FUNCTION)
 (CONCATENATE_FUNC)
 (REVERSE_FUNC)
 (SUBSTITUTE_FUNC)
 (SUBSTITUTE_CASE_FUNC)
 (NUMVALC_FUNC)
 (LOCALE_DT_FUNC)
] @function.builtin

; COPY's copybook.
(copy_statement book: (_) @string.special.path)

; Compiler directives (>>SOURCE, $SET ...) and the column-7 indicator.
(compiler_directive) @preproc
(indicator) @preproc

; Comments, and what fixed format sets aside: the sequence area (columns
; 1-6) and the identification area (73-80). AUTHOR. and its kind are
; comment entries.
[
 (comment)
 (comment_entry)
 (sequence_area)
 (identification_area)
] @comment
((comment) @comment.documentation
 (#match? @comment.documentation "^\\*>>"))
