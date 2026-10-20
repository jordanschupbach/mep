; Highlight captures for mep's Markdown block grammar (nvim-treesitter
; naming), as mep had them for the old grammar. The inline content (each
; `inline` node) is highlighted by ../tree-sitter-markdown_inline's query,
; parsed over its range (treesitter.cpp's CollectMarkdownInlineSpans).

;From nvim-treesitter/nvim-treesitter
; Per-level heading captures (@text.title.1 .. @text.title.6) so H1..H6
; can each get a distinct highlight-group color -- the grammar aliases
; all six ATX heading productions to one visible "atx_heading" node type,
; but each still has its own distinct marker child node type
; (atx_h1_marker .. atx_h6_marker) immediately preceding (inline), so a
; separate pattern per level is enough; no predicate needed. No flat
; @text.title fallback pattern is kept alongside these -- a heading node
; matches exactly one of the six marker types, so a generic
; "(atx_heading (inline) @text.title)" pattern would double-match every
; heading (once here, once there) and make which color wins
; non-deterministic (both captures would land on the identical span).
(atx_heading (atx_h1_marker) (inline) @text.title.1)
(atx_heading (atx_h2_marker) (inline) @text.title.2)
(atx_heading (atx_h3_marker) (inline) @text.title.3)
(atx_heading (atx_h4_marker) (inline) @text.title.4)
(atx_heading (atx_h5_marker) (inline) @text.title.5)
(atx_heading (atx_h6_marker) (inline) @text.title.6)

(setext_heading
  (paragraph) @text.title.1
  (setext_h1_underline))

(setext_heading
  (paragraph) @text.title.2
  (setext_h2_underline))

[
  (atx_h1_marker)
  (atx_h2_marker)
  (atx_h3_marker)
  (atx_h4_marker)
  (atx_h5_marker)
  (atx_h6_marker)
  (setext_h1_underline)
  (setext_h2_underline)
] @punctuation.special

[
  (link_title)
  (indented_code_block)
  (fenced_code_block)
] @text.literal

(fenced_code_block_delimiter) @punctuation.delimiter

(code_fence_content) @none

(link_destination) @text.uri

(link_label) @text.reference

[
  (list_marker_plus)
  (list_marker_minus)
  (list_marker_star)
  (list_marker_dot)
  (list_marker_parenthesis)
  (thematic_break)
] @punctuation.special

[
  (block_continuation)
  (block_quote_marker)
] @punctuation.special

(backslash_escape) @string.escape
