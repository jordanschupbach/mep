; A code block's body is highlighted in its own language.
(code_block
  (info language: (language) @injection.language)
  (code_content) @injection.content)

; Maths is LaTeX.
((math_content) @injection.content
  (#set! injection.language "latex"))
((latex) @injection.content
  (#set! injection.language "latex"))

; SVG and HTML written into the document are HTML.
((markup) @injection.content
  (#set! injection.language "html"))
