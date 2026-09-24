// Windowless test for math_tex.cpp -- the parse half of mep's LaTeX-math
// engine, behind every `$..$`/`\(..\)`/`$$..$$`/`\[..\]` span an org or HTML
// document is rendered with. The layout half (main.cpp) needs font metrics
// and is not reachable from here; everything this file checks is a pure
// function of the source text.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include "math_tex.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// TeX sets a literal `-` as U+2212 MINUS SIGN, not as the ASCII hyphen
// the byte would draw as in a proportional face -- so every expectation
// below that contains a minus writes it through this.
const char *const kMinus = "\xe2\x88\x92";

// The concatenated literal text of a subtree, in document order -- enough
// to say "this parsed into these glyphs" without walking the tree by hand
// in every check. Whitespace between atoms is gone by this point (the
// parser drops it; the *layout* is what decides how much air goes between
// two atoms, from their classes), so an expectation below never has any.
// A Space node -- an explicit `\,`/`\;` -- shows as `_`.
std::string Flatten(const MathNode &n) {
    std::string out;
    if (n.kind == MathKind::Frac) {
        out += "(" + Flatten(n.children[0]) + ")" + (n.frac_bar ? "/" : "atop") + "(" + Flatten(n.children[1]) + ")";
        return out;
    }
    if (n.kind == MathKind::Sqrt) return "sqrt(" + Flatten(n.children[0]) + ")";
    if (n.kind == MathKind::Space) return "_";
    if (n.kind == MathKind::Fenced) return n.open_delim + Flatten(n.children[0]) + n.close_delim;
    if (n.kind == MathKind::Accent) return "acc(" + Flatten(n.children[0]) + ")";
    if (n.kind == MathKind::Matrix) {
        out += "[";
        for (size_t k = 0; k < n.cells.size(); k++) {
            if (k != 0) out += (k % static_cast<size_t>(n.cols) == 0) ? ";" : ",";
            out += Flatten(n.cells[k]);
        }
        out += "]";
        return out;
    }
    out += n.text;
    for (const MathNode &c : n.children) out += Flatten(c);
    if (!n.sup.empty()) out += "^{" + Flatten(n.sup[0]) + "}";
    if (!n.sub.empty()) out += "_{" + Flatten(n.sub[0]) + "}";
    return out;
}

// The one term of a row that a single-atom expression parses to.
const MathNode &OnlyTerm(const MathNode &row) {
    Check(row.kind == MathKind::Row, "row.kind == MathKind::Row", __LINE__);
    Check(row.children.size() == 1, "row.children.size() == 1", __LINE__);
    return row.children[0];
}
}  // namespace

int main() {
    // --- Symbols, scripts and the basics -----------------------------
    {
        // A Greek command becomes its own glyph, not its name.
        CHECK(Flatten(ParseTexMath("\\alpha")) == "\xce\xb1");
        CHECK(Flatten(ParseTexMath("\\infty")) == "\xe2\x88\x9e");
        // Scripts attach to the atom before them, braced or not.
        CHECK(Flatten(ParseTexMath("x^2")) == "x^{2}");
        CHECK(Flatten(ParseTexMath("x_i^{2n}")) == "x^{2n}_{i}");
        CHECK(Flatten(ParseTexMath("e^{-x^2}")) == std::string("e^{") + kMinus + "x^{2}}");
        // \frac and \sqrt, with the "one token or a braced group" rule.
        CHECK(Flatten(ParseTexMath("\\frac{a+b}{c}")) == "(a+b)/(c)");
        CHECK(Flatten(ParseTexMath("\\frac12")) == "(1)/(2)");
        CHECK(Flatten(ParseTexMath("\\sqrt{x}")) == "sqrt(x)");
        // An unknown command shows its name rather than vanishing, so a
        // macro this engine does not know is at least legible.
        CHECK(Flatten(ParseTexMath("\\wobble")) == "wobble");
        // A bare variable slants and a digit does not; TeX's own Greek
        // convention is lowercase italic, uppercase upright.
        CHECK(OnlyTerm(ParseTexMath("x")).face == MathFace::Italic);
        CHECK(OnlyTerm(ParseTexMath("2")).face == MathFace::Upright);
        CHECK(OnlyTerm(ParseTexMath("\\alpha")).face == MathFace::Italic);
        CHECK(OnlyTerm(ParseTexMath("\\Gamma")).face == MathFace::Upright);
        CHECK(OnlyTerm(ParseTexMath("\\infty")).face == MathFace::Upright);
        // Empty input is an empty row, not a crash.
        CHECK(ParseTexMath("").children.empty());
    }

    // --- \over: plain TeX's infix fraction ---------------------------
    {
        // This is the one Maxima's `tex()` writes every fraction with, so
        // without it an exported Maxima result rendered the word "over" in
        // the middle of the equation.
        const MathNode over = ParseTexMath("{{x^3}\\over{3}}");
        CHECK(Flatten(over) == "(x^{3})/(3)");
        // Everything to the left of it is the numerator, however much
        // there is -- it is infix, not a two-argument command.
        CHECK(Flatten(ParseTexMath("a + b \\over c")) == "(a+b)/(c)");
        // And it binds inside its own group only.
        CHECK(Flatten(ParseTexMath("1 + {a \\over b} + 2")) == "1+(a)/(b)+2");
        // A real \frac still parses as one, side by side with it.
        CHECK(Flatten(ParseTexMath("{1 \\over 2} + \\frac{3}{4}")) == "(1)/(2)+(3)/(4)");
    }

    // --- Spacing commands --------------------------------------------
    {
        // Each of these used to render as the stray glyph its name ends in
        // -- a `\;` came out as a semicolon, which Maxima's output is full
        // of. They are space now, and the `_` below is Flatten's marker
        // for a Space node.
        CHECK(Flatten(ParseTexMath("a\\,b")) == "a_b");
        CHECK(Flatten(ParseTexMath("a\\;b")) == "a_b");
        CHECK(Flatten(ParseTexMath("a\\:b")) == "a_b");
        CHECK(Flatten(ParseTexMath("a\\!b")) == "a_b");
        CHECK(Flatten(ParseTexMath("a\\quad b")) == "a_b");
        CHECK(Flatten(ParseTexMath("a\\qquad b")) == "a_b");
        // TeX's own widths, in ems, thin < medium < thick < quad.
        auto width_of = [](const char *src) { return ParseTexMath(src).children[1].space_em; };
        CHECK(width_of("a\\,b") < width_of("a\\:b"));
        CHECK(width_of("a\\:b") < width_of("a\\;b"));
        CHECK(width_of("a\\;b") < width_of("a\\quad b"));
        // \! is the negative thin space.
        CHECK(width_of("a\\!b") < 0);
        // A literal escape is still the character itself, not a space.
        CHECK(Flatten(ParseTexMath("\\{x\\}")) == "{x}");
        CHECK(Flatten(ParseTexMath("50\\%")) == "50%");
    }

    // --- Atom classes ------------------------------------------------
    {
        CHECK(OnlyTerm(ParseTexMath("+")).cls == MathClass::Bin);
        CHECK(OnlyTerm(ParseTexMath("=")).cls == MathClass::Rel);
        CHECK(OnlyTerm(ParseTexMath("\\leq")).cls == MathClass::Rel);
        CHECK(OnlyTerm(ParseTexMath("\\times")).cls == MathClass::Bin);
        CHECK(OnlyTerm(ParseTexMath("\\int")).cls == MathClass::Op);
        CHECK(OnlyTerm(ParseTexMath("(")).cls == MathClass::Open);
        CHECK(OnlyTerm(ParseTexMath(")")).cls == MathClass::Close);
        CHECK(OnlyTerm(ParseTexMath(",")).cls == MathClass::Punct);
        CHECK(OnlyTerm(ParseTexMath("x")).cls == MathClass::Ord);
        // `/` is Ord in TeX, not Bin: `a/b` sets tight where `a + b` does not.
        CHECK(OnlyTerm(ParseTexMath("/")).cls == MathClass::Ord);
        // A brace written `\{` is a delimiter.
        CHECK(OnlyTerm(ParseTexMath("\\{")).cls == MathClass::Open);
        // A `\left..\right` pair is one Inner atom wrapping its body,
        // not two loose delimiters -- that is what lets the layout stretch
        // the fences to whatever the body turned out to need.
        {
            // The row has to outlive the reference: OnlyTerm hands back a
            // reference *into* it, which a temporary would not keep alive.
            const MathNode brace_row = ParseTexMath("\\left\\{ x \\right\\}");
            const MathNode &fenced = OnlyTerm(brace_row);
            CHECK(fenced.kind == MathKind::Fenced);
            CHECK(fenced.cls == MathClass::Inner);
            CHECK(fenced.open_delim == "{");
            CHECK(fenced.close_delim == "}");
            CHECK(Flatten(fenced) == "{x}");
        }
        // `\left.` is TeX's explicit "no delimiter on this side".
        CHECK(OnlyTerm(ParseTexMath("\\left. x \\right|")).open_delim.empty());
        // A Greek letter is ordinary, whatever else it looks like.
        CHECK(OnlyTerm(ParseTexMath("\\pi")).cls == MathClass::Ord);
    }

    // --- Inter-atom spacing ------------------------------------------
    {
        // TeXbook chapter 18's table: thin/medium/thick around operators
        // and relations, nothing between ordinary atoms. This is what the
        // difference between `dx=\sqrt{\pi}` and `dx = \sqrt{\pi}` is made
        // of, and with none of it a formula reads as one long word.
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Ord) == 0);
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Bin) == 4);
        CHECK(MathAtomSpaceUnits(MathClass::Bin, MathClass::Ord) == 4);
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Rel) == 5);
        CHECK(MathAtomSpaceUnits(MathClass::Rel, MathClass::Ord) == 5);
        CHECK(MathAtomSpaceUnits(MathClass::Op, MathClass::Ord) == 3);
        // No space before a comma, a thin one after it.
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Punct) == 0);
        CHECK(MathAtomSpaceUnits(MathClass::Punct, MathClass::Ord) == 3);
        // Nothing hugs an opening delimiter from the inside.
        CHECK(MathAtomSpaceUnits(MathClass::Open, MathClass::Ord) == 0);
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Close) == 0);
    }

    // --- Unary signs -------------------------------------------------
    {
        // TeX's rules 5 and 6: a `-` with nothing to bind on its left is a
        // sign, not a subtraction, and gets none of a Bin's air. This is
        // what keeps `\int_{-\infty}` and `e^{-x^2}` tight.
        const MathNode neg = ParseTexMath("-x");
        std::vector<MathClass> cls = MathRowClasses(neg.children);
        CHECK(cls.size() == 2);
        CHECK(cls[0] == MathClass::Ord);  // demoted from Bin
        // A real subtraction keeps its class.
        const MathNode sub = ParseTexMath("a-b");
        cls = MathRowClasses(sub.children);
        CHECK(cls[1] == MathClass::Bin);
        // After another operator or a relation, it is a sign again.
        cls = MathRowClasses(ParseTexMath("a=-b").children);
        CHECK(cls[2] == MathClass::Ord);
        cls = MathRowClasses(ParseTexMath("a+-b").children);
        CHECK(cls[2] == MathClass::Ord);
        cls = MathRowClasses(ParseTexMath("(-b").children);
        CHECK(cls[1] == MathClass::Ord);
        // Rule 6, the other side: a Bin with nothing to bind on its right.
        cls = MathRowClasses(ParseTexMath("a+=b").children);
        CHECK(cls[1] == MathClass::Ord);
        // An explicit space is glue, not an atom, so it is transparent to
        // both rules: this `+` is still a real binary operator.
        const MathNode spaced = ParseTexMath("a\\,+b");
        cls = MathRowClasses(spaced.children);
        CHECK(spaced.children[1].kind == MathKind::Space);
        CHECK(cls[2] == MathClass::Bin);
    }

    // --- The expressions this was fixed for --------------------------
    {
        // examples/test.org's own three, end to end.
        CHECK(Flatten(ParseTexMath("\\alpha + \\beta = \\gamma")) == "\xce\xb1+\xce\xb2=\xce\xb3");
        CHECK(Flatten(ParseTexMath("\\int_{-\\infty}^{\\infty} e^{-x^2} dx = \\sqrt{\\pi}")) ==
              std::string("\xe2\x88\xab^{\xe2\x88\x9e}_{") + kMinus + "\xe2\x88\x9e}e^{" + kMinus +
                  "x^{2}}dx=sqrt(\xcf\x80)");
        // Maxima's `tex()` output, the one that used to read "over".
        CHECK(Flatten(ParseTexMath("\\int  \\, x^2 + y^2 dx = x\\,y^2+{{x^3}\\over{3}} + C")) ==
              "\xe2\x88\xab_x^{2}+y^{2}dx=x_y^{2}+(x^{3})/(3)+C");
        // A malformed/half-typed expression still parses to something,
        // rather than stalling: the layout loop depends on that.
        CHECK(!ParseTexMath("\\frac{").children.empty());
        CHECK(!ParseTexMath("^").children.empty());
        CHECK(!ParseTexMath("{{{").children.empty());
        CHECK(ParseTexMath("\\over").children.size() == 1);
    }

    // --- \text and the face-changing commands ------------------------
    {
        // Whitespace inside \text is prose, and survives -- dropping it
        // ran "\text{for all }x" together as "forallx".
        CHECK(Flatten(ParseTexMath("\\text{for all }")) == "for all ");
        CHECK(Flatten(ParseTexMath("\\text{a b}x")) == "a bx");
        // ... while \mathrm's argument is still maths, so its spaces go.
        CHECK(Flatten(ParseTexMath("\\mathrm{a b}")) == "ab");
        // Every one of these sets its argument upright, which is the whole
        // point of writing them.
        for (const char *src : {"\\text{d}", "\\mathrm{d}", "\\operatorname{d}"}) {
            CHECK(OnlyTerm(ParseTexMath(src)).children[0].face == MathFace::Upright);
        }
        CHECK(OnlyTerm(ParseTexMath("\\mathbf{v}")).children[0].face == MathFace::Bold);
        // \mathbb picks the letterlike codepoint where Unicode has one --
        // `\mathbb{R}` is a real U+211D, not the letter R in some bold.
        CHECK(Flatten(ParseTexMath("\\mathbb{R}")) == "\xe2\x84\x9d");
        // An unrecognised face command used to leave its own name in the
        // equation: `x \in \mathbb{R}` read "x ∈ mathbbR".
        CHECK(Flatten(ParseTexMath("\\mathbb{R}")).find("mathbb") == std::string::npos);
    }

    // --- Operator names ----------------------------------------------
    {
        // \sin is one upright operator, not three italic variables, and
        // carries an Op atom's spacing.
        const MathNode sine_row = ParseTexMath("\\sin");
        const MathNode &sine = OnlyTerm(sine_row);
        CHECK(Flatten(sine) == "sin");
        CHECK(sine.cls == MathClass::Op);
        CHECK(sine.children[0].face == MathFace::Upright);
        CHECK(!sine.limits_above);
        // \lim and the big operators take their limits over/under in
        // display style; \int and \sin do not.
        CHECK(OnlyTerm(ParseTexMath("\\lim")).limits_above);
        CHECK(OnlyTerm(ParseTexMath("\\sum")).limits_above);
        CHECK(OnlyTerm(ParseTexMath("\\sum")).big_op);
        CHECK(OnlyTerm(ParseTexMath("\\int")).big_op);
        CHECK(!OnlyTerm(ParseTexMath("\\int")).limits_above);
        // \limits/\nolimits override that per use site.
        CHECK(!ParseTexMath("\\sum\\nolimits_{i}").children[0].limits_above);
        CHECK(ParseTexMath("\\int\\limits_{0}^{1}").children[0].limits_above);
    }

    // --- Accents, binomials, primes, environments ---------------------
    {
        CHECK(Flatten(ParseTexMath("\\hat{x}")) == "acc(x)");
        CHECK(Flatten(ParseTexMath("\\overline{AB}")) == "acc(AB)");
        CHECK(OnlyTerm(ParseTexMath("\\underline{x}")).accent_below);
        CHECK(!OnlyTerm(ParseTexMath("\\overline{x}")).accent_below);
        // \binom is a bar-less fraction inside parentheses.
        CHECK(Flatten(ParseTexMath("\\binom{n}{k}")) == "((n)atop(k))");
        // A prime binds like a superscript rather than sitting on the
        // baseline: `f'(x)` is f-prime-of-x.
        CHECK(Flatten(ParseTexMath("f'")) == "f^{\xe2\x80\xb2}");
        // Matrix environments lay out as a grid rather than leaving
        // "begin{pmatrix}" in the middle of the equation.
        const MathNode mat_row = ParseTexMath("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}");
        const MathNode mat = OnlyTerm(mat_row);
        CHECK(mat.kind == MathKind::Fenced);
        CHECK(mat.open_delim == "(");
        CHECK(mat.children[0].cols == 2);
        CHECK(mat.children[0].cells.size() == 4);
        CHECK(Flatten(mat) == "([a,b;c,d])");
        // A ragged `cases` still lays out, padded to the widest row.
        const MathNode cases_row = ParseTexMath("\\begin{cases} 1 & x > 0 \\\\ 0 \\end{cases}");
        const MathNode cases = OnlyTerm(cases_row);
        CHECK(cases.children[0].cols == 2);
        CHECK(cases.children[0].cells.size() == 4);
    }

    // --- Script-style spacing ----------------------------------------
    {
        // TeX drops the spacing table's parenthesized entries in script
        // and scriptscript style -- which is what keeps a `\sum_{i=1}^{n}`
        // subscript reading `i=1` rather than `i = 1` at a size where a
        // thick space is enormous.
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Rel, false) == 5);
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Rel, true) == 0);
        CHECK(MathAtomSpaceUnits(MathClass::Ord, MathClass::Bin, true) == 0);
        // The un-parenthesized ones survive: an Op still gets its thin space.
        CHECK(MathAtomSpaceUnits(MathClass::Op, MathClass::Ord, true) == 3);
        // An Inner atom (a \left..\right group, a fraction) spaces like
        // an Ord against its neighbours.
        CHECK(MathAtomSpaceUnits(MathClass::Inner, MathClass::Bin, false) == 4);
    }

    std::printf("math_tex tests passed\n");
    return 0;
}
