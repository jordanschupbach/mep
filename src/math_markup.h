#ifndef MEP_MATH_MARKUP_H
#define MEP_MATH_MARKUP_H

#include <string>
#include <vector>

// --- TeX maths as the office formats' own maths --------------------------------
//
// The document exporters' half of mep's maths: a TeX span (parsed by
// math_tex.h's ParseTexMath, the same tree the editor typesets) written as
// MathML (an OpenDocument formula object), as OMML (Word's and
// PowerPoint's equations), or -- where a format's text cannot hold an
// equation at all, as in an Impress text box -- as text runs: Unicode
// symbols, italic variables, and real superscripts and subscripts.
//
// Like the parser, these are pure functions of the source text. What the
// parser leaves out (a \sqrt's index, \newcommand) they cannot bring back.

// A whole MathML document for `latex`: <math> in the MathML namespace, set
// in display style when `display`, with the TeX kept as an annotation.
std::string TexToMathMl(const std::string &latex, bool display);

// An <m:oMath> element for `latex` (the caller declares the `m:` prefix,
// and wraps display maths in <m:oMathPara>). `run_props` goes verbatim
// into every <m:r> after its <m:rPr> -- PowerPoint wants an <a:rPr> there,
// Word a <w:rPr> -- and may be empty.
std::string TexToOmml(const std::string &latex, const std::string &run_props = std::string());

// The expression's MathML without the <math> around it (one element), to
// set inside a larger formula.
std::string TexToMathMlBody(const std::string &latex, bool display);

// The expression in StarMath, LibreOffice Math's own language, set as
// inline (text-style) maths: scripts beside a big operator, not over it.
// LibreOffice lays a formula out from a StarMath annotation when there is
// one, and it is the only way to choose the face of a formula's text.
std::string TexToStarMath(const std::string &latex);

// Whether the expression needs two dimensions -- a fraction, a root over
// more than one symbol, a sub- and a superscript on one base, a big
// operator's limits, a matrix, an accent -- rather than reading well as a
// line of text (TexToTextRuns).
bool TexNeedsLayout(const std::string &latex);

// One piece of the text-run spelling of an expression.
struct MathTextRun {
    std::string text;
    bool italic = false;
    bool bold = false;
    int script = 0;  // +1 superscript, -1 subscript, 0 on the baseline
};
std::vector<MathTextRun> TexToTextRuns(const std::string &latex);
// The runs' text run together: what a reader without styles would see.
std::string TexToPlainText(const std::string &latex);

// A rough size for laying the expression out, in ems of its font size:
// its width, and its height (1 for one line of symbols; a fraction, a
// matrix or a big operator's limits add to it).
// `ascent_em`, when given, is how much of the height is above the baseline
// text on the same line sits on.
void TexMathExtent(const std::string &latex, bool display, double *width_em, double *height_em, double *ascent_em = nullptr);

#endif
