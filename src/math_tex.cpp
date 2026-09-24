#include "math_tex.h"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace {

// Encodes one Unicode codepoint as UTF-8. A local copy of the same
// four-line encoder editor.cpp exposes: pulling in editor.h for it would
// drag the whole editor -- pdf/office/sheet/gfx headers included -- into a
// module whose entire point is being a leaf that a test can link on its
// own.
/**
 * @brief Encodes a Unicode codepoint as a UTF-8 string.
 * @param cp The codepoint.
 * @return Its UTF-8 encoding.
 */
std::string MathUtf8(int cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
    return out;
}

// One symbol command's expansion: the Unicode codepoint it draws as, and
// the TeX atom class that decides how much air goes around it. Every
// codepoint listed here must also be in main.cpp's kMathCodepoints (the
// baked glyph set) or it draws as a missing-glyph box.
struct MathSymbol {
    int cp;
    MathClass cls;
};

/**
 * @brief Returns the static LaTeX-command-name to symbol table, built once on first call.
 * @return Reference to the shared name-to-symbol table.
 */
const std::unordered_map<std::string, MathSymbol> &SymbolTable() {
    static const std::unordered_map<std::string, MathSymbol> kTable = {
        // --- Greek, lower and upper. Ordinary atoms, every one of them.
        {"alpha", {0x3b1, MathClass::Ord}},      {"beta", {0x3b2, MathClass::Ord}},
        {"gamma", {0x3b3, MathClass::Ord}},      {"delta", {0x3b4, MathClass::Ord}},
        {"epsilon", {0x3b5, MathClass::Ord}},    {"varepsilon", {0x3b5, MathClass::Ord}},
        {"zeta", {0x3b6, MathClass::Ord}},       {"eta", {0x3b7, MathClass::Ord}},
        {"theta", {0x3b8, MathClass::Ord}},      {"vartheta", {0x3d1, MathClass::Ord}},
        {"iota", {0x3b9, MathClass::Ord}},       {"kappa", {0x3ba, MathClass::Ord}},
        {"lambda", {0x3bb, MathClass::Ord}},     {"mu", {0x3bc, MathClass::Ord}},
        {"nu", {0x3bd, MathClass::Ord}},         {"xi", {0x3be, MathClass::Ord}},
        {"omicron", {0x3bf, MathClass::Ord}},    {"pi", {0x3c0, MathClass::Ord}},
        {"varpi", {0x3d6, MathClass::Ord}},      {"rho", {0x3c1, MathClass::Ord}},
        {"varrho", {0x3c1, MathClass::Ord}},     {"sigma", {0x3c3, MathClass::Ord}},
        {"varsigma", {0x3c2, MathClass::Ord}},   {"tau", {0x3c4, MathClass::Ord}},
        {"upsilon", {0x3c5, MathClass::Ord}},    {"phi", {0x3c6, MathClass::Ord}},
        {"varphi", {0x3c6, MathClass::Ord}},     {"chi", {0x3c7, MathClass::Ord}},
        {"psi", {0x3c8, MathClass::Ord}},        {"omega", {0x3c9, MathClass::Ord}},
        {"Gamma", {0x393, MathClass::Ord}},      {"Delta", {0x394, MathClass::Ord}},
        {"Theta", {0x398, MathClass::Ord}},      {"Lambda", {0x39b, MathClass::Ord}},
        {"Xi", {0x39e, MathClass::Ord}},         {"Pi", {0x3a0, MathClass::Ord}},
        {"Sigma", {0x3a3, MathClass::Ord}},      {"Upsilon", {0x3a5, MathClass::Ord}},
        {"Phi", {0x3a6, MathClass::Ord}},        {"Psi", {0x3a8, MathClass::Ord}},
        {"Omega", {0x3a9, MathClass::Ord}},

        // --- Binary operators.
        {"times", {0xd7, MathClass::Bin}},       {"div", {0xf7, MathClass::Bin}},
        {"pm", {0xb1, MathClass::Bin}},          {"mp", {0x2213, MathClass::Bin}},
        {"cdot", {0xb7, MathClass::Bin}},      {"circ", {0x2218, MathClass::Bin}},
        {"bullet", {0x2219, MathClass::Bin}},    {"ast", {0x2a, MathClass::Bin}},
        {"star", {0x22c6, MathClass::Bin}},      {"diamond", {0x22c4, MathClass::Bin}},
        {"otimes", {0x2297, MathClass::Bin}},    {"oplus", {0x2295, MathClass::Bin}},
        {"ominus", {0x2296, MathClass::Bin}},    {"oslash", {0x2298, MathClass::Bin}},
        {"odot", {0x2299, MathClass::Bin}},      {"cup", {0x222a, MathClass::Bin}},
        {"cap", {0x2229, MathClass::Bin}},       {"sqcup", {0x2294, MathClass::Bin}},
        {"sqcap", {0x2293, MathClass::Bin}},     {"uplus", {0x228e, MathClass::Bin}},
        {"wedge", {0x2227, MathClass::Bin}},     {"land", {0x2227, MathClass::Bin}},
        {"vee", {0x2228, MathClass::Bin}},       {"lor", {0x2228, MathClass::Bin}},
        {"setminus", {0x5c, MathClass::Bin}},  
        {"triangleleft", {0x25c1, MathClass::Bin}}, {"triangleright", {0x25b7, MathClass::Bin}},

        // --- Relations.
        {"leq", {0x2264, MathClass::Rel}},       {"le", {0x2264, MathClass::Rel}},
        {"geq", {0x2265, MathClass::Rel}},       {"ge", {0x2265, MathClass::Rel}},
        {"neq", {0x2260, MathClass::Rel}},       {"ne", {0x2260, MathClass::Rel}},
        {"approx", {0x2248, MathClass::Rel}},    {"equiv", {0x2261, MathClass::Rel}},
        {"sim", {0x223c, MathClass::Rel}},       {"simeq", {0x2243, MathClass::Rel}},
        {"cong", {0x2245, MathClass::Rel}},      {"asymp", {0x224d, MathClass::Rel}},
             
            {"ll", {0x226a, MathClass::Rel}},
        {"gg", {0x226b, MathClass::Rel}},        {"prec", {0x227a, MathClass::Rel}},
        {"succ", {0x227b, MathClass::Rel}},      
            {"in", {0x2208, MathClass::Rel}},
        {"notin", {0x2209, MathClass::Rel}},     {"ni", {0x220b, MathClass::Rel}},
        {"subset", {0x2282, MathClass::Rel}},    {"subseteq", {0x2286, MathClass::Rel}},
        {"supset", {0x2283, MathClass::Rel}},    {"supseteq", {0x2287, MathClass::Rel}},
         
        {"nsubseteq", {0x2288, MathClass::Rel}}, {"sqsubseteq", {0x2291, MathClass::Rel}},
        {"sqsupseteq", {0x2292, MathClass::Rel}}, 
        {"vdash", {0x22a2, MathClass::Rel}},     {"dashv", {0x22a3, MathClass::Rel}},
        {"perp", {0x22a5, MathClass::Rel}},      {"parallel", {0x2225, MathClass::Rel}},
        {"mid", {0x2223, MathClass::Rel}},       {"nmid", {0x2224, MathClass::Rel}},
        {"coloneqq", {0x2254, MathClass::Rel}},

        // --- Arrows (relations, all of them).
        {"to", {0x2192, MathClass::Rel}},              {"rightarrow", {0x2192, MathClass::Rel}},
        {"gets", {0x2190, MathClass::Rel}},            {"leftarrow", {0x2190, MathClass::Rel}},
        {"leftrightarrow", {0x2194, MathClass::Rel}},  {"Rightarrow", {0x21d2, MathClass::Rel}},
        {"Leftarrow", {0x21d0, MathClass::Rel}},       {"Leftrightarrow", {0x21d4, MathClass::Rel}},
        {"iff", {0x21d4, MathClass::Rel}},             {"mapsto", {0x21a6, MathClass::Rel}},
        {"hookrightarrow", {0x21aa, MathClass::Rel}},  {"hookleftarrow", {0x21a9, MathClass::Rel}},
        {"longrightarrow", {0x27f6, MathClass::Rel}},  {"longleftarrow", {0x27f5, MathClass::Rel}},
        {"longleftrightarrow", {0x27f7, MathClass::Rel}}, {"Longrightarrow", {0x21d2, MathClass::Rel}},
        {"Longleftarrow", {0x21d0, MathClass::Rel}},   {"Longleftrightarrow", {0x21d4, MathClass::Rel}},
        {"implies", {0x21d2, MathClass::Rel}},         {"impliedby", {0x21d0, MathClass::Rel}},
        {"uparrow", {0x2191, MathClass::Rel}},         {"downarrow", {0x2193, MathClass::Rel}},
        {"updownarrow", {0x2195, MathClass::Rel}},     {"nearrow", {0x2197, MathClass::Rel}},
        {"searrow", {0x2198, MathClass::Rel}},         {"swarrow", {0x2199, MathClass::Rel}},
        {"nwarrow", {0x2196, MathClass::Rel}},

        // --- Big operators. Sized up in display style; the ones whose
        // limits go over/under rather than beside are marked in BigOpTable.
        {"sum", {0x2211, MathClass::Op}},        {"prod", {0x220f, MathClass::Op}},
        {"coprod", {0x2210, MathClass::Op}},     {"int", {0x222b, MathClass::Op}},
               
        {"bigcup", {0x22c3, MathClass::Op}},     {"bigcap", {0x22c2, MathClass::Op}},
        {"bigoplus", {0x2295, MathClass::Op}},   {"bigotimes", {0x2297, MathClass::Op}},
        {"bigvee", {0x2228, MathClass::Op}},     {"bigwedge", {0x2227, MathClass::Op}},

        // --- Delimiters that are their own command.
        {"lfloor", {0x230a, MathClass::Open}},   {"rfloor", {0x230b, MathClass::Close}},
        {"lceil", {0x2308, MathClass::Open}},    {"rceil", {0x2309, MathClass::Close}},
        {"langle", {0x27e8, MathClass::Open}},   {"rangle", {0x27e9, MathClass::Close}},
        {"lbrace", {0x7b, MathClass::Open}},     {"rbrace", {0x7d, MathClass::Close}},
        {"lbrack", {0x5b, MathClass::Open}},     {"rbrack", {0x5d, MathClass::Close}},
        {"vert", {0x7c, MathClass::Ord}},        {"Vert", {0x2016, MathClass::Ord}},
        {"backslash", {0x5c, MathClass::Ord}},

        // --- Everything else: ordinary atoms.
        {"infty", {0x221e, MathClass::Ord}},     {"partial", {0x2202, MathClass::Ord}},
        {"nabla", {0x2207, MathClass::Ord}},     {"forall", {0x2200, MathClass::Ord}},
        {"exists", {0x2203, MathClass::Ord}},    {"neg", {0xac, MathClass::Ord}},
        {"lnot", {0xac, MathClass::Ord}},        {"emptyset", {0x2205, MathClass::Ord}},
        {"varnothing", {0x2205, MathClass::Ord}}, {"complement", {0x2201, MathClass::Ord}},
        {"top", {0x22a4, MathClass::Ord}},       {"bot", {0x22a5, MathClass::Ord}},
             {"triangle", {0x25b3, MathClass::Ord}},
        {"square", {0x25a1, MathClass::Ord}},    {"Box", {0x25a1, MathClass::Ord}},
        {"hbar", {0x127, MathClass::Ord}},      {"ell", {0x2113, MathClass::Ord}},
                
                {"aleph", {0x5d0, MathClass::Ord}},
        {"degree", {0xb0, MathClass::Ord}},      {"prime", {0x2032, MathClass::Ord}},
        {"dagger", {0x2020, MathClass::Ord}},    {"ddagger", {0x2021, MathClass::Ord}},
        {"surd", {0x221a, MathClass::Ord}},      {"checkmark", {0x2713, MathClass::Ord}},
        {"flat", {0x266d, MathClass::Ord}},      {"sharp", {0x266f, MathClass::Ord}},
           {"spadesuit", {0x2660, MathClass::Ord}},
         
        {"clubsuit", {0x2663, MathClass::Ord}},  {"S", {0xa7, MathClass::Ord}},
        {"P", {0xb6, MathClass::Ord}},           {"pounds", {0xa3, MathClass::Ord}},
        {"copyright", {0xa9, MathClass::Ord}},
        {"cdots", {0x22ef, MathClass::Ord}},     {"ldots", {0x2026, MathClass::Ord}},
        {"dots", {0x2026, MathClass::Ord}},      {"dotsb", {0x22ef, MathClass::Ord}},
        {"vdots", {0x22ee, MathClass::Ord}},     {"ddots", {0x22f1, MathClass::Ord}},
        {"therefore", {0x2234, MathClass::Ord}}, {"because", {0x2235, MathClass::Ord}},
    };
    return kTable;
}

// The big operators whose scripts stack over and under them in display
// style. \int and friends are big too, but their limits sit beside them --
// which is why this is a set of its own rather than "everything of class
// Op".
/**
 * @brief Returns the set of big-operator command names whose limits stack over/under them.
 * @return Reference to the shared name set.
 */
const std::unordered_set<std::string> &LimitsAboveTable() {
    static const std::unordered_set<std::string> kTable = {
        "sum", "prod", "coprod", "bigcup", "bigcap", "bigoplus", "bigotimes", "bigvee", "bigwedge",
    };
    return kTable;
}

/**
 * @brief Returns the set of command names that draw as an oversized big operator.
 * @return Reference to the shared name set.
 */
const std::unordered_set<std::string> &BigOpTable() {
    static const std::unordered_set<std::string> kTable = {
        "sum", "prod", "coprod", "int", "bigcup", "bigcap",
        "bigoplus", "bigotimes", "bigvee", "bigwedge",
    };
    return kTable;
}

// The upright multi-letter operator names (\sin, \log, \lim, ...). TeX sets
// these in roman with an Op atom's spacing around them, which is the whole
// difference between `\sin x` and the three italic variables `s`, `i`, `n`.
// The bool is whether the name takes over/under limits in display style.
/**
 * @brief Returns the table of predefined upright operator names and whether each takes
 * over/under limits in display style.
 * @return Reference to the shared name-to-limits table.
 */
const std::unordered_map<std::string, bool> &FunctionNameTable() {
    static const std::unordered_map<std::string, bool> kTable = {
        {"sin", false},  {"cos", false},    {"tan", false},    {"cot", false},   {"sec", false},
        {"csc", false},  {"arcsin", false}, {"arccos", false}, {"arctan", false}, {"sinh", false},
        {"cosh", false}, {"tanh", false},   {"coth", false},   {"exp", false},   {"log", false},
        {"ln", false},   {"lg", false},     {"det", false},    {"dim", false},   {"ker", false},
        {"deg", false},  {"hom", false},    {"arg", false},    {"Pr", false},    {"bmod", false},
        {"lim", true},   {"limsup", true},  {"liminf", true},  {"max", true},    {"min", true},
        {"sup", true},   {"inf", true},     {"gcd", true},     {"argmax", true}, {"argmin", true},
    };
    return kTable;
}

// The blackboard-bold letters that exist as single Unicode letterlike
// codepoints (and so as real glyphs in the baked math faces). The rest of
// \mathbb's alphabet lives in the Mathematical Alphanumeric Symbols plane,
// which neither embedded font has -- those fall back to upright bold,
// which reads as "a set" well enough and never draws a missing-glyph box.
/**
 * @brief Returns the map from ASCII letter to its blackboard-bold letterlike codepoint.
 * @return Reference to the shared letter-to-codepoint map.
 */
const std::unordered_map<char, int> &BlackboardTable() {
    static const std::unordered_map<char, int> kTable = {
        {'C', 0x2102}, {'H', 0x210d}, {'N', 0x2115}, {'P', 0x2119},
        {'Q', 0x211a}, {'R', 0x211d}, {'Z', 0x2124},
    };
    return kTable;
}

// The accent commands: the mark each one draws, whether it goes under the
// base rather than over it, and whether it stretches to the base's full
// width (\overline) or is a single centered glyph (\hat).
struct MathAccent {
    int cp;  // 0 = a drawn rule rather than a glyph (\overline/\underline)
    bool below;
    bool stretch;
};

/**
 * @brief Returns the static accent-command table.
 * @return Reference to the shared name-to-accent table.
 */
const std::unordered_map<std::string, MathAccent> &AccentTable() {
    static const std::unordered_map<std::string, MathAccent> kTable = {
        {"hat", {0x5e, false, false}},       {"widehat", {0x5e, false, true}},
        {"tilde", {0x7e, false, false}},     {"widetilde", {0x7e, false, true}},
        {"bar", {0, false, true}},           {"overline", {0, false, true}},
        {"underline", {0, true, true}},      {"vec", {0x2192, false, false}},
        {"dot", {0xb7, false, false}},       {"ddot", {0xa8, false, false}},
        {"check", {0x2c7, false, false}},    {"breve", {0x2d8, false, false}},
        {"acute", {0xb4, false, false}},     {"grave", {0x60, false, false}},
        {"mathring", {0xb0, false, false}},  {"overbrace", {0, false, true}},
        {"underbrace", {0, true, true}},
    };
    return kTable;
}

// The explicit spacing commands, name -> width in ems. TeX's own values:
// thin 3/18, medium 4/18, thick 5/18, and \! the negative thin. Without
// these each one used to render as the stray glyph its name happens to end
// in -- a `\;` in the middle of an equation came out as a semicolon, which
// Maxima's own `tex()` output is full of.
/**
 * @brief Returns the static lookup table mapping LaTeX spacing commands to their width in ems.
 * @return Reference to the shared spacing table.
 */
const std::unordered_map<std::string, float> &SpaceTable() {
    static const std::unordered_map<std::string, float> kTable = {
        {",", 3.0f / 18.0f},  {":", 4.0f / 18.0f}, {";", 5.0f / 18.0f},
        {"!", -3.0f / 18.0f}, {" ", 1.0f / 3.0f},  {"quad", 1.0f},
        {"qquad", 2.0f},      {"thinspace", 3.0f / 18.0f},
        {"medspace", 4.0f / 18.0f}, {"thickspace", 5.0f / 18.0f}, {"enspace", 0.5f},
    };
    return kTable;
}

// The commands that change the face of their braced argument. The bool is
// whether whitespace inside the argument survives: it does in \text (which
// is prose), and does not in \mathrm (which is still maths).
struct FaceCommand {
    MathFace face;
    bool literal_text;
};

/**
 * @brief Returns the static table of face-changing commands (\\text, \\mathrm, \\mathbf, ...).
 * @return Reference to the shared name-to-face table.
 */
const std::unordered_map<std::string, FaceCommand> &FaceTable() {
    static const std::unordered_map<std::string, FaceCommand> kTable = {
        {"text", {MathFace::Upright, true}},       {"textrm", {MathFace::Upright, true}},
        {"textnormal", {MathFace::Upright, true}}, {"mbox", {MathFace::Upright, true}},
        {"textbf", {MathFace::Bold, true}},        {"textit", {MathFace::Italic, true}},
        {"texttt", {MathFace::Mono, true}},        {"textsf", {MathFace::Sans, true}},
        {"mathrm", {MathFace::Upright, false}},    {"operatorname", {MathFace::Upright, false}},
        {"mathbf", {MathFace::Bold, false}},       {"boldsymbol", {MathFace::BoldItalic, false}},
        {"mathit", {MathFace::Italic, false}},     {"mathtt", {MathFace::Mono, false}},
        {"mathsf", {MathFace::Sans, false}},       {"mathcal", {MathFace::Italic, false}},
        {"mathscr", {MathFace::Italic, false}},    {"mathfrak", {MathFace::Upright, false}},
        {"mathnormal", {MathFace::Italic, false}},
    };
    return kTable;
}

// The matrix-like environments, each with the delimiters it fences its grid
// in and whether its cells are left-aligned (`cases`, `aligned`) rather
// than centered (`pmatrix`).
struct MatrixEnv {
    int open_cp, close_cp;  // 0 = no delimiter on that side
    bool left_align;
};

/**
 * @brief Returns the static table of supported \\begin{...} environments.
 * @return Reference to the shared environment table.
 */
const std::unordered_map<std::string, MatrixEnv> &EnvTable() {
    static const std::unordered_map<std::string, MatrixEnv> kTable = {
        {"matrix", {0, 0, false}},          {"smallmatrix", {0, 0, false}},
        {"pmatrix", {'(', ')', false}},     {"bmatrix", {'[', ']', false}},
        {"Bmatrix", {'{', '}', false}},     {"vmatrix", {'|', '|', false}},
        {"Vmatrix", {0x2016, 0x2016, false}}, {"cases", {'{', 0, true}},
        {"array", {0, 0, true}},            {"aligned", {0, 0, true}},
        {"align", {0, 0, true}},            {"align*", {0, 0, true}},
        {"alignedat", {0, 0, true}},        {"split", {0, 0, true}},
        {"gather", {0, 0, false}},          {"gathered", {0, 0, false}},
        {"eqnarray", {0, 0, true}},         {"eqnarray*", {0, 0, true}},
    };
    return kTable;
}

// Where a ParseRow call stops. The top level stops only at end of input;
// everything nested stops at whatever closed its construct, and never
// consumes that terminator itself (the caller does, so it can tell a real
// close from a run-off-the-end).
enum RowStop : unsigned {
    kStopEof = 0,
    kStopBrace = 1u << 0,  // `}`
    kStopRight = 1u << 1,  // `\right`
    kStopCell = 1u << 2,   // `&` and `\\`, inside a matrix environment
    kStopEnv = 1u << 3,    // `\end`
};

// Recursive-descent parser over the raw LaTeX source between the \(../\[..
// delimiters ExtractMathSpans already stripped -- no separate tokenizer,
// the grammar is small enough to scan character-by-character directly.
struct MathParser {
    const std::string &s;
    size_t i = 0;
    // Guards against a pathological nesting depth in malformed input
    // (`{{{{{...`) recursing the parser off the stack. Well beyond any
    // real expression's depth.
    int depth = 0;
    static constexpr int kMaxDepth = 64;

    /**
     * @brief Constructs a parser over `src`, starting at offset 0.
     * @param src The raw LaTeX math source to parse (a reference kept for the parser's lifetime).
     */
    explicit MathParser(const std::string &src) : s(src) {}

    /**
     * @brief Advances the cursor past any run of whitespace at the current position.
     */
    void SkipSpace() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    }

    /**
     * @brief Builds an explicit-space node of the given width.
     * @param em The space's width in ems (may be negative).
     * @return A Space-kind math node.
     */
    static MathNode SpaceNode(float em) {
        MathNode n;
        n.kind = MathKind::Space;
        n.space_em = em;
        return n;
    }

    // TeX sets lowercase Greek in math italic and uppercase Greek upright
    // -- the convention every paper follows, and the reason `\sqrt{\pi}`
    // in a real renderer has a slanted pi. Everything else a symbol
    // command produces (operators, arrows, delimiters) is upright.
    /**
     * @brief Returns the face a symbol codepoint is set in.
     * @param cp The symbol's codepoint.
     * @return Italic for lowercase Greek, Upright otherwise.
     */
    static MathFace SymbolFace(int cp) {
        const bool lowercase_greek = (cp >= 0x3b1 && cp <= 0x3c9) || cp == 0x3d1 || cp == 0x3d6;
        return lowercase_greek ? MathFace::Italic : MathFace::Upright;
    }

    /**
     * @brief Builds an upright Text node holding one symbol.
     * @param text The glyph's UTF-8 bytes.
     * @param cls The atom class to give it.
     * @return The Text node.
     */
    static MathNode SymbolNode(const std::string &text, MathClass cls) {
        MathNode n;
        n.kind = MathKind::Text;
        n.face = MathFace::Upright;
        n.text = text;
        n.cls = cls;
        return n;
    }

    /**
     * @brief Returns the alphabetic command name at the cursor without consuming it.
     * @return The name, or "" when the cursor is not on a `\name` command.
     */
    std::string PeekCommandName() const {
        if (i >= s.size() || s[i] != '\\') return "";
        size_t k = i + 1;
        while (k < s.size() && std::isalpha(static_cast<unsigned char>(s[k]))) k++;
        return s.substr(i + 1, k - i - 1);
    }

    /**
     * @brief Consumes a `{name}` argument, tolerating a missing brace.
     * @return The name between the braces, or "" if there was no braced group at the cursor.
     */
    std::string ParseBracedName() {
        SkipSpace();
        if (i >= s.size() || s[i] != '{') return "";
        size_t close = s.find('}', i + 1);
        if (close == std::string::npos) {
            i = s.size();
            return "";
        }
        std::string name = s.substr(i + 1, close - i - 1);
        i = close + 1;
        return name;
    }

    // The delimiter after a \left/\right/\big -- either a one-character
    // one (`(`, `[`, `|`, `/`) or a command (`\{`, `\langle`, `\|`). A `.`
    // is TeX's explicit "no delimiter on this side".
    /**
     * @brief Consumes and returns the delimiter following a \\left/\\right/\\big sizing command.
     * @return The delimiter's UTF-8 bytes, or "" for `.` (TeX's explicit null delimiter).
     */
    std::string ParseDelimiter() {
        SkipSpace();
        if (i >= s.size()) return "";
        if (s[i] == '\\') {
            const std::string name = PeekCommandName();
            if (name.empty()) {
                // A one-character escape: `\{`, `\}`, `\|`.
                i++;  // past '\'
                if (i >= s.size()) return "";
                const char c = s[i++];
                if (c == '|') return MathUtf8(0x2016);
                return std::string(1, c);
            }
            i += 1 + name.size();
            auto it = SymbolTable().find(name);
            if (it != SymbolTable().end()) return MathUtf8(it->second.cp);
            return "";
        }
        const char c = s[i++];
        if (c == '.') return "";  // \left. -- nothing drawn on this side
        return std::string(1, c);
    }

    /**
     * @brief Parses a braced group "{ row }" (consuming both braces) or, absent a brace, a
     * single ParseAtom() result -- the "one token or a braced group" argument rule shared by
     * sup/sub arguments and \\frac/\\sqrt arguments.
     * @return The parsed group's or atom's node.
     */
    MathNode ParseGroupOrAtom() {
        SkipSpace();
        if (i < s.size() && s[i] == '{') {
            i++;
            MathNode row = ParseRow(kStopBrace);
            if (i < s.size() && s[i] == '}') i++;
            return row;
        }
        return ParseAtom();
    }

    // The body of a \text{...}: prose, so whitespace in it is real and a
    // brace nests rather than ending it early. Each character becomes its
    // own upright atom (including the spaces), which is what lets the
    // layout measure and place them exactly as written.
    /**
     * @brief Parses a \\text-like command's braced body verbatim, preserving its whitespace.
     * @param face The face to set every character of the body in.
     * @return A Row of one Text node per character.
     */
    MathNode ParseLiteralText(MathFace face) {
        MathNode row;
        row.kind = MathKind::Row;
        SkipSpace();
        if (i >= s.size()) return row;
        if (s[i] != '{') {
            // `\text x` -- one token, same rule as every other argument.
            MathNode n = ParseAtom();
            n.face = face;
            row.children.push_back(std::move(n));
            return row;
        }
        i++;  // past '{'
        int nesting = 1;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '{') {
                nesting++;
                i++;
                continue;
            }
            if (c == '}') {
                if (--nesting == 0) {
                    i++;
                    break;
                }
                i++;
                continue;
            }
            if (c == '\\' && i + 1 < s.size()) {
                // Inside prose the only escapes worth honouring are the
                // literal ones (`\&`, `\_`, `\$`) and `\ `, an explicit
                // space. A command name is not prose -- drop the backslash
                // and show the name, the same fallback ParseCommand has.
                const char next = s[i + 1];
                if (!std::isalpha(static_cast<unsigned char>(next))) {
                    MathNode n;
                    n.face = face;
                    n.text = std::string(1, next);
                    row.children.push_back(std::move(n));
                    i += 2;
                    continue;
                }
                i++;
                continue;
            }
            MathNode n;
            n.face = face;
            n.text = std::string(1, c);
            row.children.push_back(std::move(n));
            i++;
        }
        return row;
    }

    // \mathrm/\mathbf/\mathbb and friends: still maths (so whitespace is
    // dropped and nested constructs still parse), just set in another face.
    /**
     * @brief Parses a face-changing command's argument as maths and restyles every Text atom in it.
     * @param face The face to apply throughout the argument.
     * @param blackboard True for \\mathbb, which substitutes letterlike codepoints where they exist.
     * @return The restyled argument, as a Row.
     */
    MathNode ParseStyledMath(MathFace face, bool blackboard) {
        MathNode grp = ParseGroupOrAtom();
        MathNode row;
        row.kind = MathKind::Row;
        if (grp.kind == MathKind::Row) {
            row.children = std::move(grp.children);
        } else {
            row.children.push_back(std::move(grp));
        }
        Restyle(row.children, face, blackboard);
        return row;
    }

    /**
     * @brief Recursively applies a face (and optional blackboard-bold substitution) to a term list.
     * @param terms The terms to restyle in place.
     * @param face The face to set.
     * @param blackboard True to substitute letterlike blackboard-bold codepoints where they exist.
     */
    static void Restyle(std::vector<MathNode> &terms, MathFace face, bool blackboard) {
        for (MathNode &t : terms) {
            if (t.kind == MathKind::Text) {
                t.face = face;
                if (blackboard && t.text.size() == 1) {
                    auto it = BlackboardTable().find(t.text[0]);
                    if (it != BlackboardTable().end()) t.text = MathUtf8(it->second);
                }
            }
            Restyle(t.children, face, blackboard);
            Restyle(t.sup, face, blackboard);
            Restyle(t.sub, face, blackboard);
            Restyle(t.cells, face, blackboard);
        }
    }

    /**
     * @brief Builds an Op-class Row spelling out an upright multi-letter operator name.
     * @param name The operator name (e.g. "sin", "lim").
     * @param limits Whether its scripts stack over/under it in display style.
     * @return The operator's Row node.
     */
    static MathNode FunctionNode(const std::string &name, bool limits) {
        MathNode row;
        row.kind = MathKind::Row;
        row.cls = MathClass::Op;
        row.limits_above = limits;
        for (char c : name) {
            MathNode g;
            g.face = MathFace::Upright;
            g.text = std::string(1, c);
            row.children.push_back(std::move(g));
        }
        return row;
    }

    // `\begin{pmatrix} a & b \\ c & d \end{pmatrix}` and friends. Cells are
    // collected row-major with the widest row setting the column count, so
    // a ragged `cases` still lays out.
    /**
     * @brief Parses a \\begin{env}...\\end{env} matrix-like environment into a Matrix node.
     * @param env The environment name, already consumed.
     * @return The Matrix node, fenced in the environment's delimiters when it has any.
     */
    MathNode ParseEnvironment(const std::string &env) {
        const auto spec = EnvTable().find(env);
        const MatrixEnv info = spec != EnvTable().end() ? spec->second : MatrixEnv{0, 0, false};
        MathNode m;
        m.kind = MathKind::Matrix;
        m.cls = MathClass::Inner;
        m.cells_left_align = info.left_align;
        // `array`'s column-spec argument (`{ccc}`) is a layout hint this
        // engine has no use for -- consume it so it doesn't parse as maths.
        if (env == "array" || env == "alignedat") ParseBracedName();
        std::vector<std::vector<MathNode>> rows;
        rows.emplace_back();
        while (true) {
            SkipSpace();
            if (i >= s.size()) break;
            if (PeekCommandName() == "end") {
                i += 4;  // past "\end"
                ParseBracedName();
                break;
            }
            MathNode cell = ParseRow(kStopCell | kStopEnv);
            rows.back().push_back(std::move(cell));
            SkipSpace();
            if (i < s.size() && s[i] == '&') {
                i++;
                continue;
            }
            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == '\\') {
                i += 2;
                rows.emplace_back();
                continue;
            }
            if (PeekCommandName() == "end") continue;
            break;  // nothing consumed the cursor -- stop rather than spin
        }
        // A trailing `\\` leaves an empty final row; it is not a row.
        if (rows.size() > 1 && rows.back().empty()) rows.pop_back();
        for (const auto &row : rows) m.cols = std::max(m.cols, static_cast<int>(row.size()));
        for (const auto &row : rows) {
            for (int c = 0; c < m.cols; c++) {
                if (c < static_cast<int>(row.size())) {
                    m.cells.push_back(row[static_cast<size_t>(c)]);
                } else {
                    MathNode blank;
                    blank.kind = MathKind::Row;
                    m.cells.push_back(std::move(blank));
                }
            }
        }
        if (info.open_cp == 0 && info.close_cp == 0) return m;
        MathNode fence;
        fence.kind = MathKind::Fenced;
        fence.cls = MathClass::Inner;
        if (info.open_cp != 0) fence.open_delim = MathUtf8(info.open_cp);
        if (info.close_cp != 0) fence.close_delim = MathUtf8(info.close_cp);
        fence.children.push_back(std::move(m));
        return fence;
    }

    /**
     * @brief Parses a backslash command at the current position and returns the resulting node.
     * @return The parsed command's resulting math node.
     */
    MathNode ParseCommand() {
        i++;  // consume '\'
        const size_t start = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
        std::string name = s.substr(start, i - start);
        if (name.empty()) {
            // A one-character command: either a spacing command (`\,`,
            // `\;`, `\!`, `\ `) or a literal escape (`\{`, `\}`, `\%`).
            // The spacing ones become real space; the rest render the
            // escaped character itself.
            if (i >= s.size()) return MathNode{};
            const std::string one(1, s[i]);
            auto sp = SpaceTable().find(one);
            if (sp != SpaceTable().end()) {
                i++;
                return SpaceNode(sp->second);
            }
            MathNode n;
            n.kind = MathKind::Text;
            n.face = MathFace::Upright;
            n.text = one == "|" ? MathUtf8(0x2016) : one;
            i++;
            // A brace written `\{` is a real delimiter, and delimiters are
            // what `\left`/`\right` wrap -- so class them, or the spacing
            // around a `\left\{ .. \right\}` group comes out wrong.
            if (n.text == "{") n.cls = MathClass::Open;
            if (n.text == "}") n.cls = MathClass::Close;
            return n;
        }
        if (auto sp = SpaceTable().find(name); sp != SpaceTable().end()) return SpaceNode(sp->second);
        // Style declarations. This engine tracks display/text/script style
        // structurally (from where an atom sits, not from a declaration),
        // so these are consumed and dropped -- which is still much better
        // than the word "displaystyle" appearing mid-equation.
        if (name == "displaystyle" || name == "textstyle" || name == "scriptstyle" ||
            name == "scriptscriptstyle" || name == "limits" || name == "nolimits" ||
            name == "nonumber" || name == "notag") {
            return MathNode{};
        }
        if (name == "left") {
            MathNode n;
            n.kind = MathKind::Fenced;
            n.cls = MathClass::Inner;
            n.open_delim = ParseDelimiter();
            n.children.push_back(ParseRow(kStopRight | kStopBrace));
            if (PeekCommandName() == "right") {
                i += 6;  // past "\right"
                n.close_delim = ParseDelimiter();
            }
            return n;
        }
        if (name == "right") {
            // Unmatched (the matching \left is gone, or was never there):
            // swallow its delimiter and draw it plain.
            const std::string delim = ParseDelimiter();
            if (delim.empty()) return MathNode{};
            return SymbolNode(delim, MathClass::Close);
        }
        if (name == "big" || name == "Big" || name == "bigg" || name == "Bigg" || name == "bigl" ||
            name == "Bigl" || name == "biggl" || name == "Biggl" || name == "bigr" || name == "Bigr" ||
            name == "biggr" || name == "Biggr" || name == "bigm" || name == "Bigm") {
            const std::string delim = ParseDelimiter();
            if (delim.empty()) return MathNode{};
            const bool closing = name.size() > 3 && name.back() == 'r';
            return SymbolNode(delim, closing ? MathClass::Close : MathClass::Open);
        }
        if (name == "begin") {
            const std::string env = ParseBracedName();
            return ParseEnvironment(env);
        }
        if (name == "end") {
            ParseBracedName();
            return MathNode{};
        }
        if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
            MathNode n;
            n.kind = MathKind::Frac;
            n.cls = MathClass::Inner;
            n.children.push_back(ParseGroupOrAtom());
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (name == "binom" || name == "dbinom" || name == "tbinom") {
            MathNode frac;
            frac.kind = MathKind::Frac;
            frac.frac_bar = false;
            frac.children.push_back(ParseGroupOrAtom());
            frac.children.push_back(ParseGroupOrAtom());
            MathNode fence;
            fence.kind = MathKind::Fenced;
            fence.cls = MathClass::Inner;
            fence.open_delim = "(";
            fence.close_delim = ")";
            fence.children.push_back(std::move(frac));
            return fence;
        }
        if (name == "sqrt") {
            MathNode n;
            n.kind = MathKind::Sqrt;
            n.cls = MathClass::Ord;
            // `\sqrt[3]{x}`: the index is not drawn (no room for it in
            // this radical), but it must not parse as maths either.
            SkipSpace();
            if (i < s.size() && s[i] == '[') {
                const size_t close = s.find(']', i);
                i = close == std::string::npos ? s.size() : close + 1;
            }
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (name == "phantom" || name == "hphantom" || name == "vphantom") {
            MathNode n;
            n.kind = MathKind::Phantom;
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (auto face = FaceTable().find(name); face != FaceTable().end()) {
            if (face->second.literal_text) return ParseLiteralText(face->second.face);
            return ParseStyledMath(face->second.face, false);
        }
        if (name == "mathbb") return ParseStyledMath(MathFace::Upright, true);
        if (auto acc = AccentTable().find(name); acc != AccentTable().end()) {
            MathNode n;
            n.kind = MathKind::Accent;
            n.accent = acc->second.cp != 0 ? MathUtf8(acc->second.cp) : "";
            n.accent_below = acc->second.below;
            n.accent_stretch = acc->second.stretch;
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (auto fn = FunctionNameTable().find(name); fn != FunctionNameTable().end()) {
            return FunctionNode(name, fn->second);
        }
        if (name == "pmod") {
            // `\pmod{n}` sets "(mod n)" with a quad of air before it.
            MathNode row;
            row.kind = MathKind::Row;
            row.children.push_back(SpaceNode(1.0f));
            row.children.push_back(SymbolNode("(", MathClass::Open));
            MathNode mod = FunctionNode("mod", false);
            row.children.push_back(std::move(mod));
            row.children.push_back(SpaceNode(3.0f / 18.0f));
            row.children.push_back(ParseGroupOrAtom());
            row.children.push_back(SymbolNode(")", MathClass::Close));
            return row;
        }
        if (auto it = SymbolTable().find(name); it != SymbolTable().end()) {
            MathNode n = SymbolNode(MathUtf8(it->second.cp), it->second.cls);
            n.face = SymbolFace(it->second.cp);
            n.big_op = BigOpTable().count(name) != 0;
            n.limits_above = LimitsAboveTable().count(name) != 0;
            return n;
        }
        // Unknown command -- show its name literally rather than dropping
        // it silently, so an unrecognized macro is at least legible/
        // debuggable instead of just vanishing from the equation.
        MathNode n;
        n.kind = MathKind::Text;
        n.face = MathFace::Upright;
        n.text = name;
        return n;
    }

    // One atom, *without* consuming a trailing ^/_ (ParseRow attaches
    // those to whatever atom precedes them).
    /**
     * @brief Parses one atom (a command, or a single literal character) without consuming a
     * trailing ^/_ -- ParseRow attaches those to whatever atom precedes them.
     * @return The parsed atom's math node.
     */
    MathNode ParseAtom() {
        SkipSpace();
        if (i >= s.size()) return MathNode{};
        if (s[i] == '\\') return ParseCommand();
        if (s[i] == '~') {
            i++;
            return SpaceNode(1.0f / 3.0f);
        }
        if (s[i] == '\'') {
            // A prime is a superscript in disguise; leaving it as a
            // baseline glyph is the one place it reads plainly wrong.
            size_t primes = 0;
            while (i < s.size() && s[i] == '\'') {
                primes++;
                i++;
            }
            MathNode n;
            n.kind = MathKind::Text;
            n.face = MathFace::Upright;
            for (size_t k = 0; k < primes; k++) n.text += MathUtf8(0x2032);
            return n;
        }
        const char c = s[i++];
        MathNode n;
        n.kind = MathKind::Text;
        n.text = std::string(1, c);
        n.face = std::isalpha(static_cast<unsigned char>(c)) != 0 ? MathFace::Italic : MathFace::Upright;
        n.cls = CharClass(c);
        // TeX sets a literal `-` as the minus sign, not the hyphen the
        // ASCII byte draws as in a proportional face.
        if (c == '-') n.text = MathUtf8(0x2212);
        return n;
    }

    // The TeX class of a literal character. `/` is deliberately Ord, not
    // Bin, the same as in TeX: `a/b` sets tight, unlike `a + b`.
    /**
     * @brief Returns the TeX atom class of a single literal character.
     * @param c The character.
     * @return Its class; Ord for anything that is not an operator, relation, delimiter or punctuation.
     */
    static MathClass CharClass(char c) {
        switch (c) {
            case '+':
            case '-':
            case '*':
                return MathClass::Bin;
            case '=':
            case '<':
            case '>':
                return MathClass::Rel;
            case '(':
            case '[':
                return MathClass::Open;
            case ')':
            case ']':
                return MathClass::Close;
            case ',':
            case ';':
                return MathClass::Punct;
            default:
                return MathClass::Ord;
        }
    }

    /**
     * @brief Checks whether the cursor sits on one of `stops`' terminators.
     * @param stops The RowStop mask the current ParseRow call was given.
     * @return True when the row should end here, without consuming anything.
     */
    bool AtRowEnd(unsigned stops) const {
        if (i >= s.size()) return true;
        if ((stops & kStopBrace) != 0 && s[i] == '}') return true;
        if ((stops & kStopCell) != 0 && s[i] == '&') return true;
        if ((stops & kStopCell) != 0 && s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') return true;
        if (s[i] != '\\') return false;
        const std::string name = PeekCommandName();
        if ((stops & kStopRight) != 0 && name == "right") return true;
        if ((stops & kStopEnv) != 0 && name == "end") return true;
        return false;
    }

    // A left-to-right sequence of atoms (each optionally followed by ^/_),
    // stopping -- without consuming -- at whichever of `stops`' terminators
    // comes first.
    /**
     * @brief Parses a left-to-right sequence of atoms, each optionally followed by ^/_, stopping
     * at (without consuming) one of `stops`' terminators. Recovers from a non-progressing parse
     * by consuming one literal byte, so malformed/in-progress input can't stall the layout loop.
     * @param stops RowStop mask naming which terminators end this row.
     * @return The parsed row's math node.
     */
    MathNode ParseRow(unsigned stops) {
        MathNode row;
        row.kind = MathKind::Row;
        if (++depth > kMaxDepth) {
            depth--;
            i = s.size();
            return row;
        }
        while (true) {
            SkipSpace();
            if (AtRowEnd(stops)) break;
            // `\over` is plain TeX's infix fraction -- everything to its
            // left in this group is the numerator and everything to its
            // right is the denominator, which is why it cannot be handled
            // in ParseCommand with the prefix commands. Maxima's own
            // `tex()` writes every fraction this way (`{{x^3}\over{3}}`),
            // so without this an exported Maxima result rendered the word
            // "over" in the middle of the equation.
            const std::string infix = PeekCommandName();
            if (infix == "over" || infix == "atop" || infix == "choose") {
                i += 1 + infix.size();
                MathNode frac;
                frac.kind = MathKind::Frac;
                frac.cls = MathClass::Inner;
                frac.frac_bar = infix == "over";
                MathNode num;
                num.kind = MathKind::Row;
                num.children = std::move(row.children);
                frac.children.push_back(std::move(num));
                frac.children.push_back(ParseRow(stops));
                MathNode wrapper;
                wrapper.kind = MathKind::Row;
                if (infix == "choose") {
                    MathNode fence;
                    fence.kind = MathKind::Fenced;
                    fence.cls = MathClass::Inner;
                    fence.open_delim = "(";
                    fence.close_delim = ")";
                    fence.children.push_back(std::move(frac));
                    wrapper.children.push_back(std::move(fence));
                } else {
                    wrapper.children.push_back(std::move(frac));
                }
                depth--;
                return wrapper;
            }
            // Every parser branch is expected to consume input, but this is
            // also exercised continuously while an Office user is typing an
            // incomplete LaTeX expression. Keep that transient, malformed
            // state fail-safe: one byte of literal text is better than an
            // accidental non-progressing render loop freezing the UI.
            const size_t before_atom = i;
            MathNode atom = ParseGroupOrAtom();
            if (i == before_atom) {
                MathNode literal;
                literal.kind = MathKind::Text;
                literal.face = MathFace::Upright;
                literal.text = std::string(1, s[i++]);
                row.children.push_back(std::move(literal));
                continue;
            }
            for (;;) {
                SkipSpace();
                if (i < s.size() && s[i] == '^') {
                    i++;
                    atom.sup.push_back(ParseGroupOrAtom());
                } else if (i < s.size() && s[i] == '_') {
                    i++;
                    atom.sub.push_back(ParseGroupOrAtom());
                } else if (i < s.size() && s[i] == '\'') {
                    // `f'(x)`: a prime binds like a superscript.
                    MathNode prime = ParseAtom();
                    if (atom.sup.empty()) {
                        MathNode holder;
                        holder.kind = MathKind::Row;
                        holder.children.push_back(std::move(prime));
                        atom.sup.push_back(std::move(holder));
                    } else {
                        atom.sup[0].children.push_back(std::move(prime));
                    }
                } else if (PeekCommandName() == "limits" || PeekCommandName() == "nolimits") {
                    const bool limits = PeekCommandName() == "limits";
                    i += limits ? 7 : 9;
                    atom.limits_above = limits;
                } else {
                    break;
                }
            }
            // An empty node is what a consumed-but-invisible command
            // (\displaystyle, a stray \end) leaves behind; it must not
            // become a zero-width Ord that the spacing rules then see.
            if (!(atom.kind == MathKind::Text && atom.text.empty() && atom.sup.empty() && atom.sub.empty())) {
                row.children.push_back(std::move(atom));
            }
        }
        depth--;
        return row;
    }
};

// TeX's inter-atom spacing table (TeXbook chapter 18), in eighteenths of
// an em: 0 none, 3 thin, 4 medium, 5 thick. Rows are the left atom's
// class, columns the right one's; the combinations TeX marks impossible
// (a Bin next to a Rel, say) are 0 here. This is what the difference
// between `dx=\sqrt{\pi}` and `dx = \sqrt{\pi}` is made of, and with no
// spacing at all a formula reads as one long word -- by some distance the
// biggest thing that made rendered maths here not look like maths.
constexpr int kMathAtomSpace[8][8] = {
    /*              Ord Op Bin Rel Open Close Punct Inner */
    /* left Ord   */ {0, 3, 4, 5, 0, 0, 0, 3},
    /* left Op    */ {3, 3, 0, 5, 0, 0, 0, 3},
    /* left Bin   */ {4, 4, 0, 0, 4, 0, 0, 4},
    /* left Rel   */ {5, 5, 0, 0, 5, 0, 0, 5},
    /* left Open  */ {0, 0, 0, 0, 0, 0, 0, 0},
    /* left Close */ {0, 3, 4, 5, 0, 0, 0, 3},
    /* left Punct */ {3, 3, 0, 3, 3, 3, 3, 3},
    /* left Inner */ {3, 3, 4, 5, 3, 0, 3, 3},
};

// The entries the TeXbook parenthesizes: spacing that applies in display
// and text style but is dropped entirely in script and scriptscript. It is
// what keeps a `\sum_{i=1}^{n}`'s limits tight instead of setting them with
// a full relation's worth of air, at a size where that air is enormous.
constexpr bool kMathAtomSpaceIsThinSpaceOnly[8][8] = {
    /*              Ord Op Bin Rel Open Close Punct Inner */
    /* left Ord   */ {false, false, true, true, false, false, false, true},
    /* left Op    */ {false, false, false, true, false, false, false, true},
    /* left Bin   */ {true, true, false, false, true, false, false, true},
    /* left Rel   */ {true, true, false, false, true, false, false, true},
    /* left Open  */ {false, false, false, false, false, false, false, false},
    /* left Close */ {false, false, true, true, false, false, false, true},
    /* left Punct */ {true, true, false, true, true, true, true, true},
    /* left Inner */ {true, false, true, true, true, false, true, true},
};

}  // namespace

MathNode ParseTexMath(const std::string &latex) {
    MathParser parser(latex);
    return parser.ParseRow(kStopEof);
}

// TeX's own two rules for when a binary operator is not really binary
// (TeXbook chapter 18, rules 5 and 6): a `-` with nothing to bind on its
// left is a sign, not a subtraction, and must not get a Bin's air around
// it. This is what keeps `e^{-x^2}` and `\int_{-\infty}` tight.
/**
 * @brief Reclassifies the Bin atoms in a term list that are really unary signs, per TeX's rules.
 * @param terms The row's terms.
 * @return One class per term, with unary Bins demoted to Ord.
 */
std::vector<MathClass> MathRowClasses(const std::vector<MathNode> &terms) {
    std::vector<MathClass> cls;
    cls.reserve(terms.size());
    for (const MathNode &t : terms) cls.push_back(t.cls);
    // An explicit space is glue, not an atom, so it is transparent to both
    // rules below: `a \, + b` still has a real binary `+`.
    auto prev_atom = [&](size_t k) -> size_t {
        while (k > 0) {
            k--;
            if (terms[k].kind != MathKind::Space) return k + 1;  // 1-based, 0 = "nothing before it"
        }
        return 0;
    };
    for (size_t k = 0; k < terms.size(); k++) {
        if (terms[k].kind == MathKind::Space) continue;
        const size_t before = prev_atom(k);
        if (cls[k] == MathClass::Bin) {
            // Rule 5: first in the list, or preceded by something nothing
            // can bind to.
            if (before == 0) {
                cls[k] = MathClass::Ord;
            } else {
                const MathClass p = cls[before - 1];
                if (p == MathClass::Bin || p == MathClass::Op || p == MathClass::Rel || p == MathClass::Open ||
                    p == MathClass::Punct) {
                    cls[k] = MathClass::Ord;
                }
            }
        } else if (cls[k] == MathClass::Rel || cls[k] == MathClass::Close || cls[k] == MathClass::Punct) {
            // Rule 6: a Bin with nothing to bind on its *right* either.
            if (before > 0 && cls[before - 1] == MathClass::Bin) cls[before - 1] = MathClass::Ord;
        }
    }
    return cls;
}

int MathAtomSpaceUnits(MathClass left, MathClass right, bool script_style) {
    const int l = static_cast<int>(left);
    const int r = static_cast<int>(right);
    if (script_style && kMathAtomSpaceIsThinSpaceOnly[l][r]) return 0;
    return kMathAtomSpace[l][r];
}

int MathAtomSpaceUnits(MathClass left, MathClass right) {
    return MathAtomSpaceUnits(left, right, false);
}
