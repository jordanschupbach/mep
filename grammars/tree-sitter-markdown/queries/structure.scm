; Document outline for mep's structure sidebar: every ATX and setext
; heading, its kind its level (heading1..heading6), its name the heading's
; text. Lines in a code block, an HTML block or front matter are never
; headings. main.cpp's mep_structure_md_items trims an ATX closing
; sequence (`## Title ##`) and computes each entry's span.

(atx_heading (atx_h1_marker) heading_content: (inline) @name) @definition.heading1
(atx_heading (atx_h2_marker) heading_content: (inline) @name) @definition.heading2
(atx_heading (atx_h3_marker) heading_content: (inline) @name) @definition.heading3
(atx_heading (atx_h4_marker) heading_content: (inline) @name) @definition.heading4
(atx_heading (atx_h5_marker) heading_content: (inline) @name) @definition.heading5
(atx_heading (atx_h6_marker) heading_content: (inline) @name) @definition.heading6
(setext_heading heading_content: (paragraph (inline) @name) (setext_h1_underline)) @definition.heading1
(setext_heading heading_content: (paragraph (inline) @name) (setext_h2_underline)) @definition.heading2
