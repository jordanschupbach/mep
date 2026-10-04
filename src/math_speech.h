#ifndef MEP_MATH_SPEECH_H
#define MEP_MATH_SPEECH_H

#include <string>

// A TeX formula as it is said aloud: what stands for a formula that has no
// alternative text of its own (mepml's \alttext) wherever one is needed --
// a tagged PDF's /Alt, the accessibility reading of a document.
//
//   \hat{\boldsymbol{\theta}}_1 = \frac{a}{b}   "theta hat 1 equals a over b"
//   (\mathbf{X}^\top \mathbf{X})^{-1}           "the quantity X transpose X, inverse"
//   \sum_{i=1}^{n} x_i^2                        "the sum from i equals 1 to n of x i squared"
//
// Not a typesetter and not a parser of all of TeX: it knows the commands
// formulas are written with (fractions, roots, scripts, accents, sums,
// Greek, relations, matrices, the font and spacing commands, which say
// nothing), says a command it does not know by its name, and never fails.
// Bold, calligraphic and upright letters are read as the letter; capitals
// are not told from small letters.
namespace mathspeech {

std::string Speak(const std::string &tex);

}  // namespace mathspeech

#endif  // MEP_MATH_SPEECH_H
