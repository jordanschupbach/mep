; Fold ranges: programs, divisions and the data division's sections (the
; procedure division's paragraphs are not nodes of their own in this
; grammar; mep-cobol-lsp folds those).
[
 (program_definition)
 (identification_division)
 (environment_division)
 (configuration_section)
 (input_output_section)
 (data_division)
 (file_section)
 (working_storage_section)
 (local_storage_section)
 (linkage_section)
 (procedure_division)
 (select_statement)
 (file_description)
] @fold
