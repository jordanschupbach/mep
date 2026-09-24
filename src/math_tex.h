#ifndef MEP_MATH_TEX_H
#define MEP_MATH_TEX_H

#include <string>
#include <vector>

// --- Mini LaTeX math parsing -----------------------------------------------
//
// The front half of mep's from-scratch, intentionally small LaTeX-math
// typesetter: the part that turns the source of a \(..\)/\[..\]/$..$/$$..$$
// span (which html_doc.cpp's ExtractMathSpans has already pulled out of
// org-mode's -- or any other MathJax-targeting page's -- exported HTML into
// a synthetic <math> DOM node) into a tree, plus the atom-class rules that
// decide how much air goes between its pieces.
//
// The *back* half -- turning this tree into glyph runs, bars and stretched
// delimiters -- stays in main.cpp, because it needs real font metrics
// (MeasureTextEx against the baked math faces) that this file deliberately
// has no access to. That split is also what makes everything here testable:
// the tree and the spacing are pure functions of the source text, and
// mep-math-tex-test pins them.
//
// Not real TeX math typesetting, and the exclusions are worth naming: no
// italic-correction or kerning tables, no line breaking inside an
// expression, no \newcommand, and no alignment across the rows of an
// `align` environment (each row is set independently). What it does cover
// -- symbols, scripts with real style tracking, fractions, radicals,
// stretchy \left..\right fences, big operators with limits, accents,
// matrix/cases environments and the TeXbook's inter-atom spacing -- is
// enough that a MathJax-targeting page reads as typeset mathematics rather
// than as raw `\alpha^2 + \beta^2` source text.

enum class MathKind {
    Text,     // one literal glyph (or, for a function name, one letter of it)
    Row,      // a left-to-right sequence, in `children`
    Frac,     // children = [numerator, denominator]
    Sqrt,     // children = [radicand]
    Space,    // explicit glue, `space_em` wide
    Fenced,   // children[0] wrapped in `open_delim`/`close_delim`, stretched to fit
    Accent,   // `accent` drawn over (or under) children[0]
    Matrix,   // `cells`, row-major, `cols` per row
    Phantom,  // children[0]'s box, reserved but not drawn
};

// TeX's atom classes -- what an atom *is*, which is what decides how much
// air goes around it (see MathAtomSpaceUnits). Set at parse time rather
// than guessed from the glyph afterwards: by then a symbol is UTF-8 bytes,
// and its LaTeX name -- the thing that actually said "this is a relation"
// -- is gone.
enum class MathClass { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

// Which face a Text atom is set in. Italic is the default because a bare
// variable slants in math mode; everything that is not a variable (digits,
// operators, symbols, \text and \mathrm runs, function names) says so.
enum class MathFace { Italic, Upright, Bold, BoldItalic, Mono, Sans };

// One node in a parsed (but not yet laid-out) math expression tree. A
// std::vector<MathNode> member on a type that also contains MathNode
// members is fine in C++17 (vector supports incomplete element types) --
// no indirection/unique_ptr needed for this self-referential shape.
struct MathNode {
    MathKind kind = MathKind::Text;
    MathClass cls = MathClass::Ord;  // this atom's TeX class, for inter-atom spacing
    float space_em = 0;              // Space kind only: width in ems (negative for \!)
    std::string text;                // Text kind only: literal glyph(s) to draw
    MathFace face = MathFace::Italic;  // Text kind only

    // A big operator (\sum, \int, \bigcup, ...): drawn oversized in display
    // style. `limits_above` is the half of that which is not about size --
    // whether this atom's scripts stack over/under it in display style
    // (\sum, \lim) or sit beside it (\int) -- so a multi-letter operator
    // name like \lim can set it without also being scaled up.
    bool big_op = false;
    bool limits_above = false;

    std::string open_delim, close_delim;  // Fenced: either may be empty (`\left.`)

    std::string accent;           // Accent: the mark to draw
    bool accent_below = false;    // \underline et al rather than \hat et al
    bool accent_stretch = false;  // \overline/\widehat: as wide as the base, not one glyph

    bool frac_bar = true;  // Frac: \binom and \atop stack with no rule

    std::vector<MathNode> cells;    // Matrix: the entries, row-major
    int cols = 0;                   // Matrix: entries per row
    bool cells_left_align = false;  // Matrix: `cases`/`aligned` rather than `pmatrix`

    std::vector<MathNode> children;  // see MathKind for each kind's meaning
    std::vector<MathNode> sup;       // trailing ^{...} attached to *this* node, 0 or 1 element (itself Row-kind)
    std::vector<MathNode> sub;       // trailing _{...} attached to *this* node, 0 or 1 element (itself Row-kind)
};

/**
 * @brief Parses one math span's raw LaTeX source into a tree.
 * @param latex The source, with its \\(..\\)/$..$ delimiters already stripped.
 * @return The parsed row. Always succeeds: an unknown command degrades to its own name as
 * literal text rather than failing, the same tolerance the rest of the HTML renderer has.
 */
MathNode ParseTexMath(const std::string &latex);

/**
 * @brief The space TeX puts between two adjacent atoms, in eighteenths of an em.
 * @param left The left atom's class.
 * @param right The right atom's class.
 * @param script_style True in script/scriptscript style, where TeX drops the table's
 * parenthesized entries (all the Bin/Rel/Punct/Inner spacing) entirely.
 * @return 0 (none), 3 (thin), 4 (medium) or 5 (thick).
 */
int MathAtomSpaceUnits(MathClass left, MathClass right, bool script_style);

/**
 * @brief The space TeX puts between two adjacent atoms in display/text style.
 * @param left The left atom's class.
 * @param right The right atom's class.
 * @return 0 (none), 3 (thin), 4 (medium) or 5 (thick).
 */
int MathAtomSpaceUnits(MathClass left, MathClass right);

/**
 * @brief Reclassifies the Bin atoms in a row that are really unary signs, per TeX's own rules.
 * @param terms The row's terms.
 * @return One class per term, with unary Bins demoted to Ord.
 */
std::vector<MathClass> MathRowClasses(const std::vector<MathNode> &terms);

#endif
