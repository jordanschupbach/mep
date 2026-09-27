; mepml highlights. Capture names follow the nvim-treesitter convention;
; mep maps them in kBuiltinSyntax's mep.ts_capture_hl.

; --- headings
(heading (h1_marker) @punctuation.special) @markup.heading.1
(heading (h2_marker) @punctuation.special) @markup.heading.2
(heading (h3_marker) @punctuation.special) @markup.heading.3
(heading (h4_marker) @punctuation.special) @markup.heading.4
(heading (h5_marker) @punctuation.special) @markup.heading.5
(heading (h6_marker) @punctuation.special) @markup.heading.6

; --- //? metadata and // comments
(meta) @keyword.directive
(meta key: (meta_key) @property)
(meta value: (meta_value) @string)
(comment) @comment
(inline_comment) @comment

; --- callouts, coloured by kind
(callout (callout_marker) @comment)
((callout kind: (callout_keyword) @comment.note)
  (#any-of? @comment.note "NOTE" "INFO" "QUESTION" "EXAMPLE"))
((callout kind: (callout_keyword) @comment.hint)
  (#any-of? @comment.hint "TIP" "HINT" "SUCCESS"))
((callout kind: (callout_keyword) @comment.warning)
  (#any-of? @comment.warning "WARNING" "CAUTION"))
((callout kind: (callout_keyword) @comment.error)
  (#any-of? @comment.error "ERROR" "DANGER"))
((callout kind: (callout_keyword) @comment.todo)
  (#any-of? @comment.todo "TODO" "FIXME" "IMPORTANT"))

; --- code blocks and results
(fence_open) @punctuation.special
(fence_close) @punctuation.special
(info (language) @label)
(option name: (option_name) @property)
(option value: (option_value) @string)
(code_content) @markup.raw.block
(results (result_begin) @comment)
(results (result_end) @comment)
(results (comment_marker) @comment)
(result_text) @markup.raw.block

; --- maths
(math_open) @punctuation.special
(math_close) @punctuation.special
(math_content) @markup.math
(latex) @markup.math

; --- directives
(import "@" @keyword.directive "import" @keyword.directive)
(image "@" @keyword.directive "image" @keyword.directive)
(caption "@" @keyword.directive "caption" @keyword.directive)
(alttext "@" @keyword.directive "alttext" @keyword.directive)
(bibliography) @keyword.directive
(toc) @keyword.directive
(citation "@" @keyword.directive "citation" @keyword.directive)
(bibtex_entry "@" @keyword.directive type: (entry_type) @keyword.directive)
(path) @string.special.path
(citation_key) @label
(field_name) @property
[(braced_value) (bare_value)] @string
(string) @string
(description) @string

; --- tables, lists, rules
(table_delimiter_row) @punctuation.special
(list_marker) @markup.list
(task_marker) @markup.list.checked
(rule) @punctuation.special

; --- inline markup
(delimiter) @punctuation.delimiter
(bold) @markup.strong
(italic) @markup.italic
(underline) @markup.underline
(strikethrough) @markup.strikethrough
(deletion) @diff.minus
(insertion) @diff.plus
(verbatim) @markup.raw
(monospace) @markup.raw
(highlight) @markup.highlight
(superscript) @markup.superscript
(subscript) @markup.subscript
(escape) @string.escape
(link (link_text) @markup.link.label)
(link (url) @markup.link.url)
[(font) (font_size) (color) (footnote) (cite) (citep)] @function.macro
(argument) @string.special
(cite (argument) @markup.link)
(citep (argument) @markup.link)
