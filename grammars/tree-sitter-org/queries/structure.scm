; Document outline for mep's structure sidebar: every headline, its kind
; its level (heading1..heading6, deeper levels heading6), its name the
; title (TODO keyword and priority included, tags not). The section is
; the definition, so a headline's span covers its subsections. Lines in a
; block are its contents, never headlines (an example block showing one).
; main.cpp's mep_structure_org_items decorates TODO headlines on top.

(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*]$")) @definition.heading1
(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*][*]$")) @definition.heading2
(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*][*][*]$")) @definition.heading3
(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*][*][*][*]$")) @definition.heading4
(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*][*][*][*][*]$")) @definition.heading5
(section headline: (headline stars: (stars) @_stars item: (item) @name)
  (#match? @_stars "^[*][*][*][*][*][*]+$")) @definition.heading6
