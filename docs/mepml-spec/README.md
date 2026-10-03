# The mepml specification

mepml separates three things, the way HTML, CSS and a browser do:

| Part | File | Says |
| --- | --- | --- |
| Structure | [structure.md](structure.md) | what a document *is*: a tree of named elements |
| Style | [style.md](style.md) | how elements *look*: the `.mepss` style sheet language |
| Rendering | [rendering.md](rendering.md) | what a renderer must do with the two |

mep is one renderer. Its built-in look is itself a style sheet
(`assets/mepml/default.mepss`), so nothing about how a mepml document is
drawn is fixed by the editor: a user sheet overrides any of it, and another
editor that implements this specification renders the same documents and
honours the same sheets.

Version: **mepml-style 1**. It describes what mep implements; what is
specified but not built yet is marked *(planned)*, and where mep's own
renderers stop short of the specification is said where it applies
(rendering.md §5 for the exports, §7 for the editor).
`mep-mepml-style-test` checks this specification against the code: every
property and every element named by one is named by the other.

A later version may add elements, parts, properties and media tags; it
will not change the meaning of a sheet written for this one. A sheet needs
no version line: what a renderer does not know, it skips (style.md §1).

The syntax of mepml itself (what text produces which element) is documented
for users in `help/mepml.org` and by example in `test.mepml`; `structure.md`
specifies only its result.
