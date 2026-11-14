// APL's built-in vocabulary: see apl_lsp_vocab.h.
#include "apl_lsp_vocab.h"

#include <cctype>

namespace apl {

namespace {

std::string Upper(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

// The backtick keys follow Dyalog's US layout, which RIDE, TryAPL and most
// APL keyboard setups share: `r is ⍴, `i is ⍳, Shift is the upper symbol.
const std::vector<Glyph> &Glyphs() {
    static const std::vector<Glyph> kGlyphs = {
        // --- scalar functions --------------------------------------------------------
        {"+", "function", "Conjugate", "Plus", "plus add conjugate", "",
         "Monadic: complex conjugate (identity on reals). Dyadic: addition."},
        {"-", "function", "Negate", "Minus", "minus subtract negate", "",
         "Monadic: negation. Dyadic: subtraction."},
        {"×", "function", "Direction (Signum)", "Times", "times multiply signum direction", "-",
         "Monadic: the sign of each number (unit direction for complex). Dyadic: multiplication."},
        {"÷", "function", "Reciprocal", "Divide", "divide reciprocal", "=",
         "Monadic: 1÷⍵. Dyadic: division (0÷0 is 1)."},
        {"⌈", "function", "Ceiling", "Maximum", "ceiling max maximum upstile", "s",
         "Monadic: round up to the next integer. Dyadic: the larger of the two arguments."},
        {"⌊", "function", "Floor", "Minimum", "floor min minimum downstile", "d",
         "Monadic: round down to the next integer. Dyadic: the smaller of the two arguments."},
        {"*", "function", "Exponential", "Power", "power exp exponential star", "p",
         "Monadic: e to the power ⍵. Dyadic: ⍺ to the power ⍵."},
        {"⍟", "function", "Natural Logarithm", "Logarithm", "log logarithm ln circlestar", "*",
         "Monadic: natural logarithm. Dyadic: logarithm of ⍵ to base ⍺."},
        {"|", "function", "Magnitude", "Residue", "stile residue magnitude abs mod", "m",
         "Monadic: absolute value. Dyadic: remainder of ⍵ divided by ⍺ (⍺|⍵)."},
        {"!", "function", "Factorial", "Binomial", "factorial binomial bang", "_",
         "Monadic: factorial (gamma of ⍵+1). Dyadic: ⍺!⍵ is the number of ways to pick ⍺ things from ⍵."},
        {"○", "function", "Pi Times", "Circular", "circle pi trig circular", "o",
         "Monadic: π×⍵. Dyadic: trigonometric function number ⍺ of ⍵ (1○ sin, 2○ cos, 3○ tan, ¯1○ arcsin, ...)."},
        {"~", "function", "Not", "Without", "tilde not without", "t",
         "Monadic: Boolean negation. Dyadic: the items of ⍺ that are not in ⍵."},
        {"∨", "function", "", "Or (GCD)", "or gcd", "9",
         "Dyadic: logical or; greatest common divisor for non-Booleans."},
        {"∧", "function", "", "And (LCM)", "and lcm", "0",
         "Dyadic: logical and; least common multiple for non-Booleans."},
        {"⍱", "function", "", "Nor", "nor", "(", "Dyadic: logical not-or."},
        {"⍲", "function", "", "Nand", "nand", ")", "Dyadic: logical not-and."},
        {"<", "function", "", "Less Than", "less lt", "3", "Dyadic: 1 where ⍺ is less than ⍵."},
        {"≤", "function", "", "Less Than or Equal", "le lessequal", "4",
         "Dyadic: 1 where ⍺ is less than or equal to ⍵."},
        {"=", "function", "", "Equal", "equal eq", "5", "Dyadic: 1 where ⍺ equals ⍵ (within ⎕CT)."},
        {"≥", "function", "", "Greater Than or Equal", "ge greaterequal", "6",
         "Dyadic: 1 where ⍺ is greater than or equal to ⍵."},
        {">", "function", "", "Greater Than", "greater gt", "7", "Dyadic: 1 where ⍺ is greater than ⍵."},
        {"≠", "function", "Unique Mask", "Not Equal", "notequal ne uniquemask", "8",
         "Monadic (Dyalog): 1 for the first occurrence of each major cell. Dyadic: 1 where ⍺ differs from ⍵."},
        {"?", "function", "Roll", "Deal", "roll deal random question", "q",
         "Monadic: a random integer from ⍳⍵ (a float in (0,1) for 0). Dyadic: ⍺ distinct random items of ⍳⍵."},
        // --- structural functions ------------------------------------------------------
        {"⍴", "function", "Shape", "Reshape", "rho shape reshape", "r",
         "Monadic: the length of each axis of ⍵. Dyadic: the items of ⍵ arranged into shape ⍺, cycling as needed."},
        {",", "function", "Ravel", "Catenate", "ravel catenate comma", "",
         "Monadic: the items of ⍵ as a vector. Dyadic: ⍺ and ⍵ joined along the last axis."},
        {"⍪", "function", "Table", "Catenate First", "table catenatefirst commabar", "<",
         "Monadic: ⍵ as a matrix (one row per major cell). Dyadic: ⍺ and ⍵ joined along the first axis."},
        {"⌽", "function", "Reverse", "Rotate", "reverse rotate circlestile", "%",
         "Monadic: ⍵ reversed along the last axis. Dyadic: ⍵ rotated ⍺ places along the last axis."},
        {"⊖", "function", "Reverse First", "Rotate First", "reversefirst rotatefirst circlebar", "&",
         "Monadic: ⍵ reversed along the first axis. Dyadic: ⍵ rotated ⍺ places along the first axis."},
        {"⍉", "function", "Transpose", "Dyadic Transpose", "transpose", "^",
         "Monadic: the axes of ⍵ reversed. Dyadic: the axes of ⍵ permuted as ⍺ says."},
        {"↑", "function", "Mix (First in GNU APL with ⎕ML)", "Take", "take mix first uparrow", "y",
         "Monadic: Dyalog mixes nested items into a higher-rank array; GNU APL returns the first item. "
         "Dyadic: the first ⍺ items (the last when negative), padding with fill."},
        {"↓", "function", "Split", "Drop", "drop split downarrow", "u",
         "Monadic: ⍵'s rows as a vector of vectors. Dyadic: ⍵ without its first ⍺ items (last when negative)."},
        {"⊂", "function", "Enclose", "Partitioned Enclose", "enclose partition leftshoe", "z",
         "Monadic: ⍵ as a scalar. Dyadic: Dyalog partitioned enclose (new partition where ⍺ increases); "
         "GNU APL partition."},
        {"⊃", "function", "Disclose / First", "Pick", "disclose first pick rightshoe", "x",
         "Monadic: Dyalog (⎕ML<2) first item; GNU APL disclose (mix). Dyadic: the item of ⍵ at path ⍺."},
        {"⊆", "function", "Nest", "Partition", "nest partition leftshoeunderbar", "Z",
         "Monadic (Dyalog): enclose ⍵ unless already nested. Dyadic: partition ⍵ by the non-zero runs of ⍺."},
        {"⊇", "function", "", "Select (GNU APL)", "select rightshoeunderbar", "",
         "Dyadic (GNU APL): select items of ⍵ by the indices in ⍺."},
        {"⌷", "function", "Materialise", "Index", "squad index materialise", "L",
         "Monadic (Dyalog): the items of ⍵. Dyadic: index ⍵ by the vector of index arrays ⍺."},
        {"⍋", "function", "Grade Up", "Dyadic Grade Up", "gradeup grade", "$",
         "Monadic: the permutation that sorts ⍵ ascending. Dyadic: grade using collating sequence ⍺."},
        {"⍒", "function", "Grade Down", "Dyadic Grade Down", "gradedown", "#",
         "Monadic: the permutation that sorts ⍵ descending. Dyadic: grade using collating sequence ⍺."},
        {"⍳", "function", "Index Generator", "Index Of", "iota index indexof interval", "i",
         "Monadic: the first ⍵ indices (from ⎕IO). Dyadic: where each item of ⍵ first occurs in ⍺."},
        {"⍸", "function", "Where", "Interval Index", "where intervalindex iotaunderbar", "I",
         "Monadic (Dyalog): the indices of the 1s in Boolean ⍵. Dyadic: which interval of sorted ⍺ each ⍵ falls in."},
        {"∊", "function", "Enlist (Type in GNU APL)", "Membership", "epsilon member membership enlist in", "e",
         "Monadic: the simple items of ⍵ as a vector. Dyadic: 1 where an item of ⍺ occurs in ⍵."},
        {"⍷", "function", "", "Find", "find epsilonunderbar", "E",
         "Dyadic: 1 where the pattern ⍺ begins in ⍵."},
        {"∪", "function", "Unique", "Union", "union unique cup", "v",
         "Monadic: ⍵ without duplicates. Dyadic: ⍺ followed by the items of ⍵ not in ⍺."},
        {"∩", "function", "", "Intersection", "intersection cap", "c",
         "Dyadic: the items of ⍺ that are also in ⍵."},
        {"⊣", "function", "Same", "Left", "left lefttack same", "|",
         "Monadic: ⍵. Dyadic: ⍺ (the left argument)."},
        {"⊢", "function", "Same", "Right", "right righttack same identity", "\\",
         "Monadic: ⍵. Dyadic: ⍵ (the right argument)."},
        {"≡", "function", "Depth", "Match", "match depth equalunderbar", ":",
         "Monadic: the nesting depth of ⍵. Dyadic: 1 if ⍺ and ⍵ are identical."},
        {"≢", "function", "Tally", "Not Match", "tally notmatch", "\"",
         "Monadic: the number of major cells of ⍵. Dyadic: 1 if ⍺ and ⍵ differ."},
        {"⍎", "function", "Execute", "Execute in namespace", "execute hydrant eval", ";",
         "Monadic: evaluate the character vector ⍵ as APL. Dyadic (Dyalog): evaluate in namespace ⍺."},
        {"⍕", "function", "Format", "Format by specification", "format thorn", "'",
         "Monadic: ⍵ as characters. Dyadic: fixed-width format with width and precision ⍺."},
        {"⌹", "function", "Matrix Inverse", "Matrix Divide", "domino matrixinverse matrixdivide", "+",
         "Monadic: matrix (pseudo-)inverse. Dyadic: ⍺⌹⍵ solves the linear system ⍵+.×X ≡ ⍺ (least squares)."},
        {"⊥", "function", "", "Decode", "decode base uptack", "b",
         "Dyadic: evaluate ⍵ as digits in the number system with radices ⍺ (2⊥1 0 1 is 5)."},
        {"⊤", "function", "", "Encode", "encode represent downtack", "n",
         "Dyadic: represent ⍵ in the number system with radices ⍺ (2 2 2⊤5 is 1 0 1)."},
        {"⍞", "function", "Character Input/Output", "", "quotequad", "{",
         "⍞ reads a line of character input; ⍞←X prints X without a trailing newline."},
        {"⍯", "function", "", "", "quadnotequal", "", "Reserved glyph (unused in GNU APL and Dyalog)."},
        // --- operators -------------------------------------------------------------------
        {"/", "operator", "Reduce", "Replicate", "reduce replicate slash compress", "",
         "Operator: f/⍵ inserts f between the items along the last axis. As a function: ⍺/⍵ copies "
         "each item of ⍵ ⍺ times."},
        {"⌿", "operator", "Reduce First", "Replicate First", "reducefirst replicatefirst slashbar", "/",
         "Operator: f⌿⍵ reduces along the first axis. As a function: replicate along the first axis."},
        {"\\", "operator", "Scan", "Expand", "scan expand backslash", "",
         "Operator: f\\⍵ is the running reduction along the last axis. As a function: ⍺\\⍵ expands ⍵ by mask ⍺."},
        {"⍀", "operator", "Scan First", "Expand First", "scanfirst expandfirst backslashbar", ".",
         "Operator: scan along the first axis. As a function: expand along the first axis."},
        {"¨", "operator", "Each", "Each", "each diaeresis dieresis", "1",
         "Operator: f¨ applies f to each item (pairwise when dyadic)."},
        {"⍨", "operator", "Selfie", "Commute", "commute selfie swap tildediaeresis", "T",
         "Operator: f⍨⍵ is ⍵ f ⍵; ⍺ f⍨ ⍵ is ⍵ f ⍺. A⍨ (Dyalog) is the constant function A."},
        {"⍣", "operator", "Power", "Power", "power powerop stardiaeresis", "P",
         "Operator: f⍣n applies f n times; f⍣g applies f until (new g old) is true (f⍣≡ is a fixpoint)."},
        {".", "operator", "", "Inner Product", "dot innerproduct", "",
         "Operator: f.g is the inner product (+.× is matrix product); ∘.g is the outer product (table)."},
        {"∘", "operator", "Jot", "Compose / Bind", "jot compose bind beside outer", "j",
         "Operator: f∘g ⍵ is f g ⍵; A∘f binds a left argument; ∘.f is the outer product."},
        {"⍤", "operator", "Rank / Atop", "Rank", "rank atop jotdiaeresis", "J",
         "Operator: f⍤k applies f to the rank-k cells; f⍤g (Dyalog) is f atop g."},
        {"⍥", "operator", "Over", "Over", "over circlediaeresis", "O",
         "Operator (Dyalog): ⍺ f⍥g ⍵ is (g ⍺) f (g ⍵)."},
        {"@", "operator", "At", "At", "at", "",
         "Operator (Dyalog): (f@i) ⍵ applies f (or puts an array) at indices or a Boolean mask in ⍵."},
        {"⌸", "operator", "Key", "Key", "key quadequal", "K",
         "Operator (Dyalog): f⌸ applies f to each unique key and the indices (or items) that have it."},
        {"⌺", "operator", "Stencil", "Stencil", "stencil quaddiamond", "~",
         "Operator (Dyalog): f⌺s applies f to each (overlapping) neighbourhood of shape s."},
        {"⍠", "operator", "Variant", "Variant", "variant quadcolon", "?",
         "Operator (Dyalog): f⍠opts calls f with options (⎕R, ⎕S, ⍳ and others)."},
        {"&", "operator", "Spawn", "Spawn", "spawn ampersand thread", "",
         "Operator (Dyalog): f& runs f in a new thread and returns the thread number."},
        {"⌶", "operator", "I-Beam", "I-Beam", "ibeam", "!",
         "Operator (Dyalog): n⌶ is system service number n (experimental and internal features)."},
        {"⍛", "operator", "Behind", "Behind", "behind jotunderbar", "",
         "Operator (Dyalog 20): ⍺ f⍛g ⍵ is (f ⍺) g ⍵."},
        // --- syntax ----------------------------------------------------------------------
        {"←", "syntax", "Assignment", "", "assign gets leftarrow", "[",
         "Assignment: name←value. Also modified assignment (x+←1), indexed (x[i]←v) and selective."},
        {"→", "syntax", "Branch", "", "branch goto rightarrow", "]",
         "Branch: →line jumps to a line (usually a label) in a tradfn; →0 or → exits."},
        {"⋄", "syntax", "Statement separator", "", "diamond", "`",
         "Statement separator: a⋄b runs a then b, as if on separate lines (GNU APL also accepts ◊)."},
        {"◊", "syntax", "Statement separator (GNU APL)", "", "lozenge", "",
         "GNU APL's statement separator, the same as ⋄."},
        {"⍝", "syntax", "Comment", "", "lamp comment", ",",
         "Comment: the rest of the line is ignored. ⍝ lines right after a function header document it."},
        {"⍬", "constant", "Zilde", "", "zilde empty", "}",
         "The empty numeric vector (0⍴0)."},
        {"⎕", "function", "Quad", "", "quad", "l",
         "⎕ reads evaluated input; ⎕←X prints X. ⎕ also starts system names (⎕IO, ⎕NA)."},
        {"∇", "syntax", "Del", "", "del nabla recursion self", "g",
         "Opens and closes a tradfn definition (∇ header ... ∇); inside a dfn, ∇ is the dfn itself (recursion)."},
        {"⍺", "syntax", "Alpha", "", "alpha left", "a",
         "In a dfn: the left argument (⍺←default gives it a default). ⍺⍺ is a dop's left operand."},
        {"⍵", "syntax", "Omega", "", "omega right", "w",
         "In a dfn: the right argument. ⍵⍵ is a dop's right operand."},
        {"¯", "syntax", "High Minus", "", "macron highminus negative", "2",
         "Marks a negative number literal: ¯3 (not the function -)."},
        {"⍫", "syntax", "Del Tilde", "", "deltilde lock", "@",
         "Closes a tradfn and locks it (∇...⍫): its source can't be displayed."},
        {"∆", "syntax", "Delta", "", "delta", "h", "A letter usable in names (∆x)."},
        {"⍙", "syntax", "Delta Underbar", "", "deltaunderbar", ">", "A letter usable in names (⍙x)."},
    };
    return kGlyphs;
}

const std::vector<SystemName> &SystemNames() {
    static const std::vector<SystemName> kNames = {
        {"⎕A", "constant", "both", "⎕A", "The 26 upper-case letters A-Z."},
        {"⎕AI", "variable", "both", "⎕AI", "Accounting information: user, CPU time, connect time, keying time."},
        {"⎕AN", "variable", "dyalog", "⎕AN", "Account name (the user name)."},
        {"⎕ARG", "variable", "gnu", "⎕ARG", "The command-line arguments of the apl process."},
        {"⎕AT", "function", "dyalog", "R←{X}⎕AT Y", "Attributes of the functions and operators named in Y."},
        {"⎕ATX", "function", "dyalog", "R←X ⎕ATX Y", "Extended attributes of the named objects."},
        {"⎕AV", "constant", "both", "⎕AV", "The atomic vector: the classic APL character set."},
        {"⎕AVU", "variable", "dyalog", "⎕AVU", "The Unicode code points behind ⎕AV."},
        {"⎕BASE", "namespace", "dyalog", "⎕BASE.name", "In a derived class: the base class's member."},
        {"⎕C", "function", "dyalog", "R←{X}⎕C Y", "Case conversion: fold (X absent), lower (X=0) or upper (X=1)."},
        {"⎕CLASS", "function", "dyalog", "R←⎕CLASS Y", "The class hierarchy of an instance or class."},
        {"⎕CLEAR", "constant", "dyalog", "⎕CLEAR", "A clear workspace."},
        {"⎕CMD", "function", "dyalog", "R←⎕CMD Y", "Run an operating-system command (same as ⎕SH on Unix)."},
        {"⎕CR", "function", "both", "R←⎕CR 'name'", "Canonical representation: the source of a function as a matrix."},
        {"⎕CS", "function", "dyalog", "{R}←⎕CS Y", "Change space: make the namespace Y current."},
        {"⎕CSV", "function", "dyalog", "R←{X}⎕CSV Y", "Read or write comma-separated values."},
        {"⎕CT", "variable", "both", "⎕CT", "Comparison tolerance for = < ⌊ ⍳ and the others (default 1E¯14)."},
        {"⎕CY", "function", "dyalog", "{X}⎕CY 'ws'", "Copy objects from a saved workspace."},
        {"⎕D", "constant", "both", "⎕D", "The digits '0123456789'."},
        {"⎕DCT", "variable", "dyalog", "⎕DCT", "Decimal comparison tolerance (when ⎕FR is 1287)."},
        {"⎕DF", "function", "dyalog", "{R}←⎕DF Y", "Display form: set how a namespace displays."},
        {"⎕DIV", "variable", "dyalog", "⎕DIV", "Division method: 1 makes x÷0 return 0."},
        {"⎕DL", "function", "both", "R←⎕DL Y", "Delay: wait Y seconds; returns the time waited."},
        {"⎕DM", "variable", "dyalog", "⎕DM", "Diagnostic message of the last error."},
        {"⎕DMX", "namespace", "dyalog", "⎕DMX", "Extended diagnostic information about the last error."},
        {"⎕DQ", "function", "dyalog", "R←⎕DQ Y", "Dequeue GUI events."},
        {"⎕DR", "function", "dyalog", "R←{X}⎕DR Y", "Data representation: type code of Y, or convert Y to type X."},
        {"⎕DT", "function", "dyalog", "R←X ⎕DT Y", "Convert between date-time formats."},
        {"⎕EA", "function", "gnu", "R←A ⎕EA B", "Execute alternate: ⍎B, and ⍎A if that fails."},
        {"⎕EB", "function", "gnu", "R←A ⎕EB B", "Execute both: ⍎A then ⍎B."},
        {"⎕EC", "function", "gnu", "R←⎕EC B", "Execute controlled: ⍎B returning a return code, a value and the result."},
        {"⎕ED", "function", "dyalog", "{R}←{X}⎕ED Y", "Open the editor on the names in Y."},
        {"⎕EM", "function", "both", "R←⎕EM Y", "Event message: the error text for event number Y (GNU: last error)."},
        {"⎕EN", "variable", "dyalog", "⎕EN", "Event number of the last error."},
        {"⎕ENV", "function", "gnu", "R←⎕ENV 'prefix'", "Environment variables whose names start with prefix."},
        {"⎕ES", "function", "both", "{X}⎕ES Y", "Event simulate: signal an error with message X/number Y."},
        {"⎕ET", "variable", "gnu", "⎕ET", "Event type of the last error."},
        {"⎕EX", "function", "both", "{R}←⎕EX Y", "Expunge: erase the named objects."},
        {"⎕EXCEPTION", "variable", "dyalog", "⎕EXCEPTION", "The .NET exception behind the last error."},
        {"⎕EXPORT", "function", "dyalog", "R←{X}⎕EXPORT Y", "Export type of the named functions."},
        {"⎕FAPPEND", "function", "dyalog", "{R}←X ⎕FAPPEND Y", "Append array X to component file tie Y."},
        {"⎕FAVAIL", "constant", "dyalog", "⎕FAVAIL", "1 when component files are available."},
        {"⎕FC", "variable", "gnu", "⎕FC", "Format control: characters ⍕ uses for decimal point, comma, fill, ..."},
        {"⎕FCHK", "function", "dyalog", "R←{X}⎕FCHK Y", "Check and repair a component file."},
        {"⎕FCOPY", "function", "dyalog", "R←X ⎕FCOPY Y", "Copy a component file."},
        {"⎕FCREATE", "function", "dyalog", "{R}←X ⎕FCREATE Y", "Create a component file."},
        {"⎕FDROP", "function", "dyalog", "{R}←⎕FDROP Y", "Drop components from a component file."},
        {"⎕FERASE", "function", "dyalog", "{R}←X ⎕FERASE Y", "Erase a component file."},
        {"⎕FFT", "function", "gnu", "R←{X}⎕FFT Y", "Fast Fourier transform (GNU APL, with libfftw3)."},
        {"⎕FHIST", "function", "dyalog", "R←⎕FHIST Y", "Component file history."},
        {"⎕FHOLD", "function", "dyalog", "{R}←{X}⎕FHOLD Y", "Hold component files (a lock)."},
        {"⎕FIO", "function", "gnu", "R←{A}⎕FIO[N] B", "File I/O: the C library's fopen/fread/... by function number N."},
        {"⎕FIX", "function", "dyalog", "{R}←{X}⎕FIX Y", "Fix (define) the script in Y: functions, namespaces, classes."},
        {"⎕FLIB", "function", "dyalog", "R←⎕FLIB Y", "The component files in a directory."},
        {"⎕FMT", "function", "dyalog", "R←{X}⎕FMT Y", "Format by specification (F, I, E, G, A qualifiers)."},
        {"⎕FNAMES", "function", "dyalog", "R←⎕FNAMES", "Names of the tied component files."},
        {"⎕FNUMS", "function", "dyalog", "R←⎕FNUMS", "Tie numbers of the tied component files."},
        {"⎕FPROPS", "function", "dyalog", "R←X ⎕FPROPS Y", "Component file properties."},
        {"⎕FR", "variable", "dyalog", "⎕FR", "Floating-point representation: 645 (binary64) or 1287 (decimal128)."},
        {"⎕FRDAC", "function", "dyalog", "R←⎕FRDAC Y", "Component file access matrix."},
        {"⎕FRDCI", "function", "dyalog", "R←⎕FRDCI Y", "Component information."},
        {"⎕FREAD", "function", "dyalog", "R←⎕FREAD Y", "Read a component."},
        {"⎕FRENAME", "function", "dyalog", "{R}←X ⎕FRENAME Y", "Rename a component file."},
        {"⎕FREPLACE", "function", "dyalog", "{R}←X ⎕FREPLACE Y", "Replace a component."},
        {"⎕FRESIZE", "function", "dyalog", "{R}←X ⎕FRESIZE Y", "Set a component file's size limit."},
        {"⎕FSIZE", "function", "dyalog", "R←⎕FSIZE Y", "Component file size information."},
        {"⎕FSTAC", "function", "dyalog", "{R}←X ⎕FSTAC Y", "Set a component file's access matrix."},
        {"⎕FSTIE", "function", "dyalog", "{R}←X ⎕FSTIE Y", "Share-tie a component file."},
        {"⎕FTIE", "function", "dyalog", "{R}←X ⎕FTIE Y", "Tie a component file exclusively."},
        {"⎕FUNTIE", "function", "dyalog", "{R}←⎕FUNTIE Y", "Untie component files."},
        {"⎕FX", "function", "both", "R←{X}⎕FX Y",
         "Fix: define a function from its lines Y. GNU APL: 'lib.so' ⎕FX 'name' loads a native function."},
        {"⎕GTK", "function", "gnu", "R←{A}⎕GTK B", "GTK+ GUI interface (GNU APL)."},
        {"⎕INP", "function", "gnu", "R←{A}⎕INP B", "Input from the script up to an end marker (here-documents)."},
        {"⎕INSTANCES", "function", "dyalog", "R←⎕INSTANCES Y", "The instances of a class."},
        {"⎕IO", "variable", "both", "⎕IO", "Index origin: 1 (default) or 0. ⍳, indexing and ⍋ count from it."},
        {"⎕JSON", "function", "both", "R←{X}⎕JSON Y", "Convert between JSON text and APL arrays/namespaces."},
        {"⎕KL", "function", "dyalog", "R←⎕KL Y", "Key label."},
        {"⎕L", "variable", "gnu", "⎕L", "In GNU APL: the left argument of the failed function (after an error)."},
        {"⎕LC", "variable", "both", "⎕LC", "Line counter: the current line numbers of the stack."},
        {"⎕LOAD", "function", "dyalog", "⎕LOAD 'ws'", "Load a workspace."},
        {"⎕LOCK", "function", "dyalog", "{X}⎕LOCK Y", "Lock functions (hide their source)."},
        {"⎕LX", "variable", "both", "⎕LX", "Latent expression: run when the workspace is loaded."},
        {"⎕MAP", "function", "both", "R←{X}⎕MAP Y", "Map a file into an array (Dyalog); GNU APL: map values by a table."},
        {"⎕MKDIR", "function", "dyalog", "{R}←{X}⎕MKDIR Y", "Create a directory."},
        {"⎕ML", "variable", "dyalog", "⎕ML", "Migration level: 0-3, the APL2 compatibility of ↑ ⊃ ∊ and others."},
        {"⎕MONITOR", "function", "dyalog", "R←⎕MONITOR Y", "Monitor function lines (timing)."},
        {"⎕NA", "function", "both", "{R}←{X}⎕NA 'decl'",
         "Name association: bind a function in a shared library ('I4 lib|fn I4 <0T') as an APL function. "
         "datamunge's Dyalog binding uses it."},
        {"⎕NAPPEND", "function", "dyalog", "{R}←X ⎕NAPPEND Y", "Append to a native file."},
        {"⎕NC", "function", "both", "R←⎕NC Y", "Name class: 0 undefined, 2 variable, 3 function, 4 operator, 9 ref."},
        {"⎕NCOPY", "function", "dyalog", "{R}←{X}⎕NCOPY Y", "Copy native files."},
        {"⎕NCREATE", "function", "dyalog", "R←X ⎕NCREATE Y", "Create and tie a native file."},
        {"⎕NDELETE", "function", "dyalog", "{R}←{X}⎕NDELETE Y", "Delete a file or directory."},
        {"⎕NERASE", "function", "dyalog", "{R}←X ⎕NERASE Y", "Erase a tied native file."},
        {"⎕NEW", "function", "dyalog", "R←⎕NEW Y", "Create an instance of a class."},
        {"⎕NEXISTS", "function", "dyalog", "R←{X}⎕NEXISTS Y", "1 for each named file that exists."},
        {"⎕NGET", "function", "dyalog", "R←{X}⎕NGET Y", "Read a text file: (content encoding newline)."},
        {"⎕NINFO", "function", "dyalog", "R←{X}⎕NINFO Y", "File information (name, type, size, times), with wildcards."},
        {"⎕NL", "function", "both", "R←{X}⎕NL Y", "Name list: the names of class Y (2 variables, 3 functions, ...)."},
        {"⎕NLOCK", "function", "dyalog", "{R}←X ⎕NLOCK Y", "Lock a region of a native file."},
        {"⎕NMOVE", "function", "dyalog", "{R}←{X}⎕NMOVE Y", "Move native files."},
        {"⎕NNAMES", "function", "dyalog", "R←⎕NNAMES", "Names of the tied native files."},
        {"⎕NNUMS", "function", "dyalog", "R←⎕NNUMS", "Tie numbers of the tied native files."},
        {"⎕NPARTS", "function", "dyalog", "R←{X}⎕NPARTS Y", "Split a file name into (path base extension)."},
        {"⎕NPUT", "function", "dyalog", "{R}←X ⎕NPUT Y", "Write a text file."},
        {"⎕NQ", "function", "dyalog", "{R}←{X}⎕NQ Y", "Enqueue a GUI event."},
        {"⎕NR", "function", "dyalog", "R←⎕NR Y", "Nested representation: a function's source as a vector of lines."},
        {"⎕NREAD", "function", "dyalog", "R←⎕NREAD Y", "Read from a native file."},
        {"⎕NRENAME", "function", "dyalog", "{R}←X ⎕NRENAME Y", "Rename a native file."},
        {"⎕NREPLACE", "function", "dyalog", "{R}←X ⎕NREPLACE Y", "Write into a native file at an offset."},
        {"⎕NRESIZE", "function", "dyalog", "{R}←X ⎕NRESIZE Y", "Resize a native file."},
        {"⎕NS", "function", "dyalog", "R←{X}⎕NS Y", "Create a namespace (with copies of the names in Y)."},
        {"⎕NSI", "variable", "dyalog", "⎕NSI", "Namespace indicator: the space each stack frame was called from."},
        {"⎕NSIZE", "function", "dyalog", "R←⎕NSIZE Y", "Size of a tied native file."},
        {"⎕NTIE", "function", "dyalog", "{R}←X ⎕NTIE Y", "Tie a native file."},
        {"⎕NULL", "constant", "dyalog", "⎕NULL", "The null item (for .NET, JSON and ⎕NA)."},
        {"⎕NUNTIE", "function", "dyalog", "{R}←⎕NUNTIE Y", "Untie native files."},
        {"⎕NXLATE", "function", "dyalog", "R←{X}⎕NXLATE Y", "Native file translation table."},
        {"⎕OFF", "function", "dyalog", "⎕OFF", "End the session (with an exit code: ⎕OFF 1)."},
        {"⎕OPT", "operator", "dyalog", "f⎕OPT opts", "Variant (the same as ⍠)."},
        {"⎕OR", "function", "dyalog", "R←⎕OR Y", "Object representation of a function."},
        {"⎕PATH", "variable", "dyalog", "⎕PATH", "Search path: namespaces searched for names not found locally."},
        {"⎕PFKEY", "function", "dyalog", "R←{X}⎕PFKEY Y", "Programmable function keys."},
        {"⎕PLOT", "function", "gnu", "R←{A}⎕PLOT B", "Plot data in a window (GNU APL)."},
        {"⎕PP", "variable", "both", "⎕PP", "Print precision: significant digits displayed (default 10)."},
        {"⎕PR", "variable", "gnu", "⎕PR", "Prompt replacement character for ⍞."},
        {"⎕PROFILE", "function", "dyalog", "R←{X}⎕PROFILE Y", "Profile application CPU and elapsed time."},
        {"⎕PS", "variable", "gnu", "⎕PS", "Print style (boxing of nested arrays) in GNU APL."},
        {"⎕PW", "variable", "both", "⎕PW", "Print width: the session's line width (default 80)."},
        {"⎕R", "operator", "both", "R←X(⎕R opts)Y",
         "Dyalog: replace (PCRE regular expressions). GNU APL: the right argument of the failed function."},
        {"⎕RE", "function", "gnu", "R←A ⎕RE[flags] B", "Regular expressions (GNU APL, libpcre2)."},
        {"⎕REFS", "function", "dyalog", "R←⎕REFS Y", "The names a function references."},
        {"⎕RL", "variable", "both", "⎕RL", "Random link: the seed of ? (roll and deal)."},
        {"⎕RSI", "variable", "dyalog", "⎕RSI", "Space indicator: references to the calling spaces."},
        {"⎕RTL", "variable", "dyalog", "⎕RTL", "Response time limit for ⍞ and ⎕ input."},
        {"⎕S", "operator", "dyalog", "R←X(⎕S opts)Y", "Search (PCRE regular expressions)."},
        {"⎕SAVE", "function", "dyalog", "{R}←⎕SAVE Y", "Save the workspace."},
        {"⎕SD", "constant", "dyalog", "⎕SD", "Screen dimensions."},
        {"⎕SE", "namespace", "dyalog", "⎕SE", "The session namespace."},
        {"⎕SH", "function", "dyalog", "R←⎕SH Y", "Run a shell command; returns its output lines."},
        {"⎕SHADOW", "function", "dyalog", "⎕SHADOW Y", "Localise names dynamically."},
        {"⎕SI", "function", "both", "R←⎕SI", "State indicator: the names of the suspended functions."},
        {"⎕SIGNAL", "function", "dyalog", "{X}⎕SIGNAL Y", "Signal an event: error number Y, message X."},
        {"⎕SIZE", "function", "dyalog", "R←⎕SIZE Y", "The bytes the named objects use."},
        {"⎕SM", "variable", "dyalog", "⎕SM", "Screen map."},
        {"⎕SQL", "function", "gnu", "R←{A}⎕SQL[N] B", "SQL database access (GNU APL: SQLite and PostgreSQL)."},
        {"⎕SR", "function", "dyalog", "R←⎕SR Y", "Screen read."},
        {"⎕SRC", "function", "dyalog", "R←⎕SRC Y", "The source of a scripted namespace or class."},
        {"⎕STACK", "variable", "dyalog", "⎕STACK", "The state indicator with stack details."},
        {"⎕STATE", "function", "dyalog", "R←⎕STATE Y", "The state of a name: its value at each stack level."},
        {"⎕STOP", "function", "both", "R←{X}⎕STOP Y", "Stop control: the lines of function Y that suspend."},
        {"⎕SVC", "function", "both", "R←{X}⎕SVC Y", "Shared variable access control."},
        {"⎕SVE", "variable", "gnu", "⎕SVE", "Shared variable event."},
        {"⎕SVO", "function", "both", "R←{X}⎕SVO Y", "Shared variable offer."},
        {"⎕SVQ", "function", "both", "R←⎕SVQ Y", "Shared variable query."},
        {"⎕SVR", "function", "both", "R←⎕SVR Y", "Shared variable retraction."},
        {"⎕SVS", "function", "both", "R←⎕SVS Y", "Shared variable state."},
        {"⎕SYL", "variable", "gnu", "⎕SYL", "System limits of GNU APL (and their current values)."},
        {"⎕TALLOC", "function", "dyalog", "R←{X}⎕TALLOC Y", "Allocate tokens for thread synchronisation."},
        {"⎕TC", "constant", "both", "⎕TC", "Terminal control characters: backspace, line feed, newline."},
        {"⎕TCNUMS", "function", "dyalog", "R←⎕TCNUMS Y", "Thread child numbers."},
        {"⎕TGET", "function", "dyalog", "R←{X}⎕TGET Y", "Get tokens from the token pool."},
        {"⎕THIS", "namespace", "dyalog", "⎕THIS", "A reference to the current namespace or instance."},
        {"⎕TID", "variable", "dyalog", "⎕TID", "The current thread's number."},
        {"⎕TKILL", "function", "dyalog", "{R}←{X}⎕TKILL Y", "Kill threads."},
        {"⎕TNAME", "variable", "dyalog", "⎕TNAME", "The current thread's name."},
        {"⎕TNUMS", "variable", "dyalog", "⎕TNUMS", "The numbers of all threads."},
        {"⎕TPOOL", "variable", "dyalog", "⎕TPOOL", "The token pool."},
        {"⎕TPUT", "function", "dyalog", "{R}←{X}⎕TPUT Y", "Put tokens into the pool."},
        {"⎕TRACE", "function", "both", "R←{X}⎕TRACE Y", "Trace control: the lines of function Y whose results print."},
        {"⎕TRAP", "variable", "dyalog", "⎕TRAP", "Event trap specifications."},
        {"⎕TREQ", "function", "dyalog", "R←⎕TREQ Y", "Token requests of threads."},
        {"⎕TS", "variable", "both", "⎕TS", "Time stamp: year month day hour minute second millisecond."},
        {"⎕TSYNC", "function", "dyalog", "R←⎕TSYNC Y", "Wait for threads to finish."},
        {"⎕TZ", "variable", "gnu", "⎕TZ", "Time zone offset in hours (GNU APL)."},
        {"⎕UCS", "function", "both", "R←⎕UCS Y", "Unicode: code points to characters and back."},
        {"⎕UL", "variable", "gnu", "⎕UL", "User load: the number of APL users."},
        {"⎕USING", "variable", "dyalog", "⎕USING", ".NET namespaces searched for classes."},
        {"⎕VFI", "function", "dyalog", "R←{X}⎕VFI Y", "Verify and fix input: (valid-mask values) of numbers in text."},
        {"⎕VGET", "function", "dyalog", "R←{X}⎕VGET Y", "Get the values of names in a namespace."},
        {"⎕VR", "function", "dyalog", "R←⎕VR Y", "Vector representation of a function (with ∇ lines)."},
        {"⎕VSET", "function", "dyalog", "{R}←{X}⎕VSET Y", "Set the values of names in a namespace."},
        {"⎕WA", "variable", "both", "⎕WA", "Workspace available: free memory in bytes."},
        {"⎕WC", "function", "dyalog", "{R}←{X}⎕WC Y", "Create a GUI object."},
        {"⎕WG", "function", "dyalog", "R←{X}⎕WG Y", "Get GUI object properties."},
        {"⎕WN", "function", "dyalog", "R←{X}⎕WN Y", "GUI object names."},
        {"⎕WS", "function", "dyalog", "{R}←{X}⎕WS Y", "Set GUI object properties."},
        {"⎕WSID", "variable", "dyalog", "⎕WSID", "Workspace identification: its file name."},
        {"⎕WX", "variable", "dyalog", "⎕WX", "Expose GUI properties as names."},
        {"⎕X", "variable", "gnu", "⎕X", "In GNU APL: the axis argument of the failed function."},
        {"⎕XML", "function", "both", "R←{X}⎕XML Y", "Convert between XML text and an APL matrix."},
        {"⎕XSI", "variable", "dyalog", "⎕XSI", "Extended state indicator: full names of the stack's functions."},
        {"⎕XT", "function", "dyalog", "{R}←{X}⎕XT Y", "Associate an external variable with a file."},
        {"⎕DLX", "function", "gnu", "R←{A}⎕DLX B", "Donald Knuth's dancing links (exact cover) in GNU APL."},
        {"⎕CCR", "function", "gnu", "R←A ⎕CCR B", "Character conversion between code pages (GNU APL)."},
    };
    return kNames;
}

const std::vector<SystemCommand> &SystemCommands() {
    static const std::vector<SystemCommand> kCommands = {
        {")CLEAR", "both", "Replace the active workspace with a clear one."},
        {")CONTINUE", "gnu", "Save the workspace as CONTINUE and end the session."},
        {")COPY", "both", "Copy objects from a saved workspace: )COPY ws [names]."},
        {")CS", "dyalog", "Change the current namespace: )CS #.ns."},
        {")DROP", "both", "Delete a saved workspace."},
        {")ED", "dyalog", "Edit objects: )ED name."},
        {")ERASE", "both", "Erase objects: )ERASE names."},
        {")FNS", "both", "List the functions in the workspace (or namespace)."},
        {")HELP", "gnu", "Show help on a primitive or the list of commands."},
        {")HOST", "gnu", "Run a shell command: )HOST ls."},
        {")IN", "gnu", "Read an APL transfer file."},
        {")LIB", "both", "List the saved workspaces in a library."},
        {")LOAD", "both", "Load a saved workspace: )LOAD ws."},
        {")MORE", "gnu", "Show more information about the last error."},
        {")NS", "dyalog", "Create a namespace or show the current one."},
        {")OBJECTS", "dyalog", "List the namespaces, classes and interfaces."},
        {")OFF", "both", "End the APL session."},
        {")OPS", "both", "List the operators in the workspace."},
        {")OUT", "gnu", "Write an APL transfer file."},
        {")PCOPY", "both", "Protected copy: copy objects without overwriting existing ones."},
        {")RESET", "both", "Clear the state indicator."},
        {")SAVE", "both", "Save the active workspace."},
        {")SH", "dyalog", "Run a shell command."},
        {")SI", "both", "Show the state indicator."},
        {")SIC", "both", "Clear the state indicator (GNU: same as )RESET)."},
        {")SINL", "both", "Show the state indicator with local names."},
        {")SIS", "gnu", "Show the state indicator with statements."},
        {")SYMBOLS", "both", "Show or set the symbol table size."},
        {")VARS", "both", "List the variables in the workspace."},
        {")WSID", "both", "Show or set the workspace name."},
        {")XLOAD", "dyalog", "Load a workspace without running ⎕LX."},
    };
    return kCommands;
}

const std::vector<ControlWord> &ControlWords() {
    static const std::vector<ControlWord> kWords = {
        {":If", "`:If cond` ... [`:ElseIf cond` ...] [`:Else` ...] `:EndIf`: run a block when cond is 1."},
        {":ElseIf", "Part of :If: an alternative condition."},
        {":Else", "The block run when no :If/:ElseIf/:Case matched."},
        {":EndIf", "Ends an :If block."},
        {":AndIf", "Adds a condition to the :If (or :ElseIf) above, evaluated only when it is true."},
        {":OrIf", "Adds an alternative condition to the :If (or :ElseIf) above."},
        {":For", "`:For var :In array` ... `:EndFor`: run a block once for each item."},
        {":In", "Part of :For: the array iterated over."},
        {":InEach", "Part of :For with several variables: iterate over the arrays in step."},
        {":EndFor", "Ends a :For block."},
        {":While", "`:While cond` ... `:EndWhile` (or `:Until cond`): repeat while cond is 1."},
        {":EndWhile", "Ends a :While block."},
        {":Repeat", "`:Repeat` ... `:Until cond` (or `:EndRepeat`): repeat until cond is 1."},
        {":Until", "Ends a :Repeat or :While block with a condition tested at the end."},
        {":EndRepeat", "Ends a :Repeat block (forever, until :Leave)."},
        {":Select", "`:Select value` `:Case v` ... `:CaseList vs` ... `:Else` ... `:EndSelect`."},
        {":Case", "Part of :Select or :Trap: the block run when the value matches."},
        {":CaseList", "Part of :Select: the block run when the value is one of the list."},
        {":EndSelect", "Ends a :Select block."},
        {":Trap", "`:Trap codes` ... [`:Case codes` ...] [`:Else` ...] `:EndTrap`: catch errors."},
        {":EndTrap", "Ends a :Trap block."},
        {":With", "`:With ns` ... `:EndWith`: run a block with ns as the current space."},
        {":EndWith", "Ends a :With block."},
        {":Hold", "`:Hold tokens` ... `:EndHold`: run a block holding tokens (threads)."},
        {":EndHold", "Ends a :Hold block."},
        {":End", "Ends the innermost control structure."},
        {":Leave", "Leave the innermost :For, :While or :Repeat loop."},
        {":Continue", "Go to the next iteration of the innermost loop."},
        {":Return", "Return from the function (→0)."},
        {":GoTo", "`:GoTo label`: branch to a label (→label)."},
        {":Namespace", "`:Namespace name` ... `:EndNamespace`: a scripted namespace."},
        {":EndNamespace", "Ends a scripted namespace."},
        {":Class", "`:Class name : base` ... `:EndClass`: a class."},
        {":EndClass", "Ends a class."},
        {":Interface", "`:Interface name` ... `:EndInterface`: an interface."},
        {":EndInterface", "Ends an interface."},
        {":Property", "`:Property name` ... `:EndProperty`: a property with get/set functions."},
        {":EndProperty", "Ends a property."},
        {":Field", "`:Field [Public] [Shared] name←value`: a class field."},
        {":Access", "`:Access Public|Private [Shared|Instance]`: who may call this member."},
        {":Implements", "`:Implements Constructor|Destructor|Method|Trigger`."},
        {":Include", "`:Include ns`: include a namespace's functions in a class."},
        {":Require", "`:Require file://path`: load a script first."},
        {":Section", "`:Section name` ... `:EndSection`: a named, foldable section (no effect on running)."},
        {":EndSection", "Ends a section."},
        {":Disposable", "`:Disposable objects` ... `:EndDisposable`: dispose .NET objects at the end."},
        {":EndDisposable", "Ends a :Disposable block."},
    };
    return kWords;
}

const Glyph *FindGlyph(const std::string &glyph) {
    for (const Glyph &g : Glyphs()) {
        if (glyph == g.glyph) return &g;
    }
    return nullptr;
}

const SystemName *FindSystemName(const std::string &name) {
    std::string key = name;
    if (key.rfind("⎕", 0) != 0) key = "⎕" + key;
    key = Upper(key);
    for (const SystemName &s : SystemNames()) {
        if (Upper(s.name) == key) return &s;
    }
    return nullptr;
}

const ControlWord *FindControlWord(const std::string &name) {
    const std::string key = Upper(name);
    for (const ControlWord &w : ControlWords()) {
        if (Upper(w.name) == key) return &w;
    }
    return nullptr;
}

const SystemCommand *FindSystemCommand(const std::string &name) {
    const std::string key = Upper(name);
    for (const SystemCommand &c : SystemCommands()) {
        if (c.name == key) return &c;
    }
    return nullptr;
}

std::string GlyphMarkdown(const Glyph &g) {
    std::string out = "**" + std::string(g.glyph) + "**";
    std::string forms;
    if (g.monadic[0] && g.dyadic[0]) {
        forms = std::string(g.monadic) + " / " + g.dyadic;
    } else {
        forms = g.monadic[0] ? g.monadic : g.dyadic;
    }
    if (!forms.empty()) out += " " + forms;
    out += " *(" + std::string(g.kind) + ")*\n\n";
    if (std::string(g.kind) == "function" || std::string(g.kind) == "operator") {
        if (g.monadic[0]) out += "- monadic: " + std::string(g.monadic) + "\n";
        if (g.dyadic[0]) out += "- dyadic: " + std::string(g.dyadic) + "\n";
        out += "\n";
    }
    out += g.doc;
    if (g.key[0]) out += std::string("\n\nType: `` `") + g.key + " `` (or a name: " + g.names + ")";
    else if (g.names[0]) out += std::string("\n\nComplete from: ") + g.names;
    return out;
}

std::string SystemNameMarkdown(const SystemName &s) {
    std::string dialect = std::string(s.dialect) == "both"  ? "GNU APL and Dyalog"
                          : std::string(s.dialect) == "gnu" ? "GNU APL"
                                                             : "Dyalog";
    return "```apl\n" + std::string(s.syntax) + "\n```\n**" + s.name + "** *(system " + s.kind + ", " + dialect +
           ")*\n\n" + s.doc;
}

std::string GlyphForKey(char key) {
    for (const Glyph &g : Glyphs()) {
        if (g.key[0] == key && g.key[1] == '\0') return g.glyph;
    }
    return std::string();
}

}  // namespace apl
