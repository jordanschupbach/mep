#!/usr/bin/env bash
# Generate the real-world PDF fixtures mep's PDF tests spot-check against.
#
# Why generated and not committed: .gitignore ignores both `test/` and
# `*.pdf`, and the repo's existing convention (see src/pdf_text_test.cpp's
# own top comment and CheckRealFixture) is an opt-in, local-only
# test/pdf_fixtures/ directory passed to the test binary as argv, with
# `pdftotext`'s independent extraction stored beside it as an oracle. This
# script is the committed half of that: run it once and the fixtures and
# their oracles exist; the binaries themselves never enter git history.
#
# Each fixture targets a specific, identified failure mode rather than
# being "a PDF with some text in it" -- the point is to cover the font
# and layout shapes a hand-assembled byte-string fixture cannot produce:
# genuine Computer Modern Type 1 programs, a CID font, a Type 3 bitmap
# font, a tagged list carrying /ActualText, stream order that disagrees
# with visual order.
#
# Usage: tools/pdf_make_fixtures.sh [outdir]      (default test/pdf_fixtures)

set -euo pipefail

OUT="${1:-test/pdf_fixtures}"
ORACLE="$OUT/oracle"
mkdir -p "$OUT" "$ORACLE"
FAILED=""
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }

for t in pdflatex pdftotext; do
    have "$t" || { echo "missing required tool: $t" >&2; exit 1; }
done

# Build one fixture from a LaTeX body. $1 = name, $2 = engine, rest = body
# on stdin. Runs twice so labels/refs settle, and keeps the engine quiet
# unless it actually fails.
build() {
    local name="$1" engine="$2"
    local tex="$WORK/$name.tex"
    cat > "$tex"
    if ! (cd "$WORK" && "$engine" -interaction=batchmode -halt-on-error "$name.tex" >/dev/null 2>&1 \
          && "$engine" -interaction=batchmode -halt-on-error "$name.tex" >/dev/null 2>&1); then
        echo "  !! $name: $engine failed; see $WORK/$name.log" >&2
        sed -n '/^!/,+6p' "$WORK/$name.log" >&2 || true
        FAILED="$FAILED $name"
        return 0
    fi
    cp "$WORK/$name.pdf" "$OUT/$name.pdf"
    mkdir -p "$ORACLE/$name"
    # Two oracles: -layout preserves the visual arrangement (what mep's
    # own visual-order extraction aims at), raw is the reading order.
    pdftotext -layout "$OUT/$name.pdf" "$ORACLE/$name/text.txt"
    pdftotext "$OUT/$name.pdf" "$ORACLE/$name/text_raw.txt"
    if have pdffonts; then pdffonts "$OUT/$name.pdf" > "$ORACLE/$name/fonts.txt"; fi
    echo "  ok $name.pdf ($(wc -c < "$OUT/$name.pdf") bytes)"
}

echo "generating fixtures into $OUT"

# ---------------------------------------------------------------------
# bullets_cm -- itemize/enumerate/nested, default Computer Modern Type 1.
# The reported bug: highlighting a bulleted item and asking a model about
# it yielded only the bullet. A list item is short and indented, so the
# display-math row classifier's !full_measure test is automatically true
# for it, and the marker is set from a different font at a different size
# than the body, which supplies the off-baseline glyph the rest of that
# test needs.
# ---------------------------------------------------------------------
build bullets_cm pdflatex <<'TEX'
\documentclass[11pt]{article}
\usepackage[margin=1in]{geometry}
\pagestyle{empty}
\begin{document}
\section*{Bulleted and numbered lists}

Ordinary prose precedes the list so the page has a real text measure to
compare indented rows against.

\begin{itemize}
  \item The first bulleted item contains enough words to be a sentence.
  \item Short item.
  \item A nested list follows this item.
  \begin{itemize}
    \item Nested bullet one with several words of its own.
    \item Nested bullet two.
  \end{itemize}
  \item A final bulleted item after the nesting closes again.
\end{itemize}

\begin{enumerate}
  \item Numbered items use a different marker shape entirely.
  \item The second numbered item is also reasonably long.
  \item[] An item with an empty optional label.
\end{enumerate}

\begin{description}
  \item[Term] A description list labels its items with words.
  \item[Another term] Which gives a bold label and indented body text.
\end{description}

Closing prose after the lists, again running the full measure.
\end{document}
TEX

# ---------------------------------------------------------------------
# display_math -- the equation shapes whose glyph names live outside the
# Latin-1 subset of the AGL that pdf_encodings.cpp actually carries:
# summation/integral/radical/brace size variants from CMEX, Greek from
# CMMI, set and logic operators from CMSY.
# ---------------------------------------------------------------------
build display_math pdflatex <<'TEX'
\documentclass[11pt]{article}
\usepackage[margin=1in]{geometry}
\usepackage{amsmath,amssymb}
\pagestyle{empty}
\begin{document}
\section*{Displayed mathematics}

A summation with limits above and below, which puts three baselines in
one row:
\begin{equation}
  S_n = \sum_{i=1}^{n} \frac{x_i - \mu}{\sigma} \label{eq:sum}
\end{equation}

An integral with a big radical and big delimiters:
\begin{equation}
  I = \int_{0}^{\infty} \sqrt{\frac{1 + t^2}{1 - t^2}}\, dt
    = \left( \frac{\pi}{2} \right)^{1/2}
\end{equation}

Greek variables, set operators and arrows together:
\begin{equation}
  \forall \alpha \in A,\; \exists \beta \in B : \alpha + \beta \leq \gamma
  \quad \Longrightarrow \quad \theta \to \lambda
\end{equation}

A matrix inside big brackets, and a binomial:
\begin{equation}
  M = \begin{pmatrix} a & b \\ c & d \end{pmatrix}, \qquad
  \binom{n}{k} = \frac{n!}{k!\,(n-k)!}
\end{equation}

Aligned equations across several lines:
\begin{align}
  \nabla \cdot \mathbf{E} &= \frac{\rho}{\varepsilon_0} \\
  \nabla \times \mathbf{B} &= \mu_0 \mathbf{J} + \mu_0 \varepsilon_0 \frac{\partial \mathbf{E}}{\partial t}
\end{align}

A limit, whose operator name and its own subscript sit on different
baselines:
\[
  \lim_{\Delta t \to 0} \Pr\{ S_{t + \Delta t} \mid S_t, C \} = \exp(Q \Delta t)
\]
\end{document}
TEX

# ---------------------------------------------------------------------
# inline_math -- the classifier's negative case. Every row here is prose
# running the full measure that happens to carry a subscript or a
# superscript. These must stay navigable: flagging them as displayed
# equations is what makes a maths book's body text unselectable.
# ---------------------------------------------------------------------
build inline_math pdflatex <<'TEX'
\documentclass[11pt]{article}
\usepackage[margin=1in]{geometry}
\usepackage{amsmath,amssymb}
\pagestyle{empty}
\begin{document}
\section*{Inline mathematics in running prose}

We write $x_i$ for the $i$th observation and $x_i^2$ for its square, so
that the estimator $\hat{\theta}_n = n^{-1} \sum_i x_i$ converges in the
usual sense as $n \to \infty$ under the stated assumptions on the tails.

The exponent in $e^{-\lambda t}$ and the subscript in $\sigma_{ij}$ both
sit off the main baseline of this paragraph, and yet every line of it
runs from the left margin to the right one exactly as plain prose does,
which is the property that has to keep them navigable.

Where a sentence mentions $\alpha$, $\beta$ and $\gamma$ in passing, and
then refers to $\mathbb{R}^d$ and to a set $\{1, \dots, n\}$, the row is
still a row of text rather than a displayed formula, however many of its
glyphs come from a mathematics font rather than the body font.
\end{document}
TEX

# ---------------------------------------------------------------------
# twocolumn_header -- content-stream order that disagrees with visual
# order. A running head plus two columns plus footnotes is the shape
# pdf_text.h describes: the producer emits body, then header, then notes.
# A mouse drag resolves a contiguous run in stream order, so on a page
# like this it selects things the user never dragged over.
# ---------------------------------------------------------------------
build twocolumn_header pdflatex <<'TEX'
\documentclass[11pt,twocolumn]{article}
\usepackage[margin=0.9in]{geometry}
\usepackage{fancyhdr}
\pagestyle{fancy}
\newcommand{\lipsumlike}{\newcommand{\fillpara}{The argument continues at
some length, with enough ordinary sentences to push the text past the
bottom of the page and well onto the one after it, so that the break
falls in the middle of a paragraph rather than neatly between two. }%
\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara
\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara\fillpara}
\fancyhead[L]{A Running Header That Is Not Body Text}
\fancyfoot[C]{\thepage}
\begin{document}
\section{First column material}
This paragraph belongs to the first column and should read as one
continuous passage when selected, with no header text spliced into the
middle of it.\footnote{A footnote, which the producer emits after the
body text of the page even though it appears below it.}

Another paragraph of first-column text, long enough to be sure the
column actually fills and wraps onto several lines of its own before the
column break arrives.

\section{Second column material}
The second column's text must not interleave with the first column's
when a reader drags across either one, even though both were emitted
into the same content stream.\footnote{A second footnote, emitted later
still.}

More second-column prose, again long enough to wrap, so that a drag over
part of it has several lines to span.

\section{Material that runs over the page break}
This paragraph is deliberately long enough to continue onto the next
page, because a selection has to be able to follow it there: a reader
quoting a passage that spans a break wants the passage, not the half of
it that happens to fit. \lipsumlike

\end{document}
TEX

# ---------------------------------------------------------------------
# ligatures_hyphen -- ff/fi/ffi ligature glyphs (whose /ToUnicode is a
# multi-character string) and words broken over a line end, which
# JoinGlyphText is supposed to put back together.
# ---------------------------------------------------------------------
build ligatures_hyphen pdflatex <<'TEX'
\documentclass[11pt]{article}
\usepackage[margin=2.6in]{geometry}
\pagestyle{empty}
\begin{document}
\noindent
The office staffing difficulties affected the efficiency of the
classification workflow, and the ramifications of insufficient
scaffolding were sufficiently difficult to quantify that the
investigators reclassified the affected office files.

\medskip
\noindent
Anglo-Saxon and twenty-five are genuinely hyphenated compounds and must
keep their hyphens, unlike a word merely broken across a line end.
\end{document}
TEX

# ---------------------------------------------------------------------
# rotated -- /Rotate 90 plus text set at an angle, exercising the
# page-to-device matrix and the isotropic glyph-box approximation, which
# pdf_content.cpp documents as exact only for unrotated text.
# ---------------------------------------------------------------------
build rotated pdflatex <<'TEX'
\documentclass[11pt]{article}
\usepackage[margin=1in]{geometry}
\usepackage{pdflscape,rotating}
\pagestyle{empty}
\begin{document}
\begin{landscape}
\section*{A landscape page}
This page carries a /Rotate entry, so its width and height swap and the
page-to-device matrix is not the identity-shaped one.
\end{landscape}
\newpage
\section*{Rotated text runs}
\begin{turn}{30}
Text set at thirty degrees to the baseline.
\end{turn}

\vspace{2cm}
\begin{turn}{90}
Text set vertically up the page.
\end{turn}
\end{document}
TEX

# ---------------------------------------------------------------------
# cid_nounicode -- a CID-keyed font via a Unicode engine. GetUnicodeText
# gates its encoding fallback on !is_composite_, so a composite font
# whose /ToUnicode is absent yields no text at all. xelatex produces
# Identity-H CID fonts; the no-ToUnicode variant is built by stripping
# the CMap afterwards (see strip_tounicode below).
# ---------------------------------------------------------------------
if have xelatex; then
    build cid_unicode xelatex <<'TEX' || true
\documentclass[11pt]{article}
\usepackage[margin=1in]{geometry}
\usepackage{fontspec}
\setmainfont{DejaVu Serif}[Scale=1.0]
\pagestyle{empty}
\begin{document}
\section*{A CID-keyed font}
This page is typeset by a Unicode engine, so its fonts are CID-keyed with
an Identity-H encoding rather than simple single-byte fonts.

\begin{itemize}
  \item A bulleted item in a CID font.
  \item Another item, with accented text: na\"ive, caf\'e, Stra\ss e.
\end{itemize}
\end{document}
TEX
else
    echo "  -- skipping cid_unicode (no xelatex)" >&2
fi

# ---------------------------------------------------------------------
# type3_bitmap -- a Type 3 font, whose glyphs are content-stream
# procedures (here ImageMask bitmaps) rather than outlines. Authored by
# hand because no modern engine emits Type 3 text fonts, yet 79
# instances turned up in a 120-file sample of real library PDFs -- and a
# bitmap glyph is the classic "the maths is low-resolution" case, since
# it has one fixed resolution and cannot sharpen when zoomed.
# ---------------------------------------------------------------------
oracle_for() {
    local name="$1"
    mkdir -p "$ORACLE/$name"
    pdftotext -layout "$OUT/$name.pdf" "$ORACLE/$name/text.txt" 2>/dev/null || true
    pdftotext "$OUT/$name.pdf" "$ORACLE/$name/text_raw.txt" 2>/dev/null || true
    if have pdffonts; then pdffonts "$OUT/$name.pdf" > "$ORACLE/$name/fonts.txt" 2>/dev/null || true; fi
    echo "  ok $name.pdf ($(wc -c < "$OUT/$name.pdf") bytes)"
}

# ---------------------------------------------------------------------
# bullets_tagged -- a tagged list whose middle item carries one
# /ActualText across both its marker and its body. Authored by hand: no
# LaTeX engine emits this, but Word, InDesign and accessibility
# remediation tools do, and it is the shape behind the reported bug --
# honouring such a span erases the item, so a highlighted bullet point
# reached an AI prompt as nothing but its bullet. poppler erases it too.
# ---------------------------------------------------------------------
# ---------------------------------------------------------------------
# scan_bitonal -- a high-resolution 1-bit stencil minified onto a letter
# page, i.e. the shape of a scanned book. Authored by hand because no
# LaTeX fixture produces it and real scans cannot be committed. It is
# the case image *minification* matters for: four or five source pixels
# per device pixel, where point sampling discards most of them and
# breaks up thin strokes.
# ---------------------------------------------------------------------
# ---------------------------------------------------------------------
# degenerate_text -- a page that looks typeset but whose text extracts
# as nonsense, because its font's encoding sends almost every code to
# the same two or three glyphs. Reproduces The Art of Computer
# Programming vol. 4A, where every font is a Type 3 bitmap with no
# /ToUnicode: nothing is missing, the glyphs map to lies. Quoting that
# to a model is worse than quoting nothing, so src/text_quality.h
# detects it and the page is sent as a picture instead.
# ---------------------------------------------------------------------
if python3 "$(dirname "$0")/pdf_make_degenerate.py" "$OUT/degenerate_text.pdf"; then
    oracle_for degenerate_text
else
    echo "  !! degenerate_text: generator failed" >&2
    FAILED="$FAILED degenerate_text"
fi

if python3 "$(dirname "$0")/pdf_make_scan.py" "$OUT/scan_bitonal.pdf"; then
    oracle_for scan_bitonal
else
    echo "  !! scan_bitonal: generator failed" >&2
    FAILED="$FAILED scan_bitonal"
fi

if python3 "$(dirname "$0")/pdf_make_tagged.py" "$OUT/bullets_tagged.pdf"; then
    oracle_for bullets_tagged
else
    echo "  !! bullets_tagged: generator failed" >&2
    FAILED="$FAILED bullets_tagged"
fi

if python3 "$(dirname "$0")/pdf_make_type3.py" "$OUT/type3_bitmap.pdf"; then
    oracle_for type3_bitmap
else
    echo "  !! type3_bitmap: generator failed" >&2
    FAILED="$FAILED type3_bitmap"
fi

echo
echo "fixtures in $OUT:"
ls -la "$OUT"/*.pdf 2>/dev/null || echo "  (none built)"
echo
echo "oracles in $ORACLE (pdftotext's own extraction, for diffing)."
if [ -n "$FAILED" ]; then echo; echo "FAILED to build:$FAILED" >&2; fi
echo "Spot-check them with:  ./build/native-dev/mep-pdf-text-test $OUT/*.pdf"
