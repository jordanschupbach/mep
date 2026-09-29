// Office maths as TeX (office_math.h) -- mepml's importers and the
// presentation editor's readers share it.
#include "office_math.h"

#include <cctype>
#include <map>
#include <vector>

namespace officemath {

namespace {

std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// One character (or name) of maths text as TeX.
std::string TexSymbols(const std::string &s) {
    static const std::map<std::string, std::string> kSym = {
        {"α", "\\alpha"},  {"β", "\\beta"},     {"γ", "\\gamma"},  {"δ", "\\delta"},  {"ε", "\\epsilon"}, {"ζ", "\\zeta"},
        {"η", "\\eta"},    {"θ", "\\theta"},    {"ι", "\\iota"},   {"κ", "\\kappa"},  {"λ", "\\lambda"},  {"μ", "\\mu"},
        {"ν", "\\nu"},     {"ξ", "\\xi"},       {"π", "\\pi"},     {"ρ", "\\rho"},    {"σ", "\\sigma"},   {"τ", "\\tau"},
        {"υ", "\\upsilon"}, {"φ", "\\phi"},     {"χ", "\\chi"},    {"ψ", "\\psi"},    {"ω", "\\omega"},   {"Γ", "\\Gamma"},
        {"Δ", "\\Delta"},  {"Θ", "\\Theta"},    {"Λ", "\\Lambda"}, {"Ξ", "\\Xi"},     {"Π", "\\Pi"},      {"Σ", "\\Sigma"},
        {"Φ", "\\Phi"},    {"Ψ", "\\Psi"},      {"Ω", "\\Omega"},  {"∞", "\\infty"},  {"→", "\\to"},      {"←", "\\leftarrow"},
        {"⇒", "\\Rightarrow"}, {"⇔", "\\Leftrightarrow"}, {"≤", "\\le"}, {"≥", "\\ge"}, {"≠", "\\ne"}, {"≈", "\\approx"},
        {"±", "\\pm"},     {"∓", "\\mp"},       {"×", "\\times"},  {"÷", "\\div"},    {"·", "\\cdot"},    {"⋅", "\\cdot"},
        {"∂", "\\partial"}, {"∇", "\\nabla"},   {"∈", "\\in"},     {"∉", "\\notin"},  {"⊂", "\\subset"},  {"⊆", "\\subseteq"},
        {"∪", "\\cup"},    {"∩", "\\cap"},      {"∀", "\\forall"}, {"∃", "\\exists"}, {"∅", "\\emptyset"}, {"∑", "\\sum"},
        {"∏", "\\prod"},   {"∫", "\\int"},      {"∬", "\\iint"},   {"∮", "\\oint"},   {"√", "\\surd"},    {"…", "\\ldots"},
        {"⋯", "\\cdots"},  {"−", "-"},          {"∗", "*"},        {"′", "'"},        {"≡", "\\equiv"},   {"∝", "\\propto"},
        {"ℝ", "\\mathbb{R}"}, {"ℕ", "\\mathbb{N}"}, {"ℤ", "\\mathbb{Z}"}, {"ℚ", "\\mathbb{Q}"}, {"ℂ", "\\mathbb{C}"},
        {"⊤", "\\top"},  {"⊥", "\\perp"},     {"∘", "\\circ"},  {"∣", "\\mid"},    {"‖", "\\|"},      {"⟨", "\\langle"},
        {"⟩", "\\rangle"}, {"⊕", "\\oplus"},  {"⊗", "\\otimes"}, {"∧", "\\wedge"}, {"∨", "\\vee"},    {"¬", "\\neg"},
        {"∖", "\\setminus"}, {"ℓ", "\\ell"},  {"⊃", "\\supset"}, {"⊇", "\\supseteq"}, {"↦", "\\mapsto"}, {"⇐", "\\Leftarrow"},
        {"↔", "\\leftrightarrow"}, {"≪", "\\ll"}, {"≫", "\\gg"}, {"∼", "\\sim"}, {"≃", "\\simeq"},  {"≅", "\\cong"},
        {"∠", "\\angle"}, {"ħ", "\\hbar"},    {"∗", "\\ast"},
        // Spaces, as TeX's own.
        {"\u2009", "\\,"}, {"\u2005", "\\:"}, {"\u2004", "\\;"}, {"\u2002", "\\ "}, {"\u2003", "\\quad "}, {"\u00A0", "~"},
        {"\u200B", ""}, {"\u2061", ""}, {"\u2062", ""}};
    static const char *kFuncs[] = {"sin", "cos", "tan", "log", "ln", "exp", "lim", "max", "min", "sup", "inf", "det", "sinh", "cosh", "tanh"};
    for (const char *f : kFuncs)
        if (s == f) return "\\" + s;
    std::string o;
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        const std::string ch = s.substr(i, n);
        auto it = kSym.find(ch);
        if (it != kSym.end()) {
            o += it->second;
            // A command followed by a letter needs a space between.
            if (i + n < s.size() && std::isalpha(static_cast<unsigned char>(s[i + n])) && it->second[0] == '\\') o += ' ';
        } else {
            o += ch;
        }
        i += n;
    }
    return o;
}

std::string Local(const xml::xml_node &n) {
    const std::string nm = n.name();
    const size_t colon = nm.find(':');
    return colon == std::string::npos ? nm : nm.substr(colon + 1);
}

// Pieces of TeX run together: a command's name must not run on into a
// letter after it (\subseteq then B is `\subseteq B`, not `\subseteqB`).
void Join(std::string &o, const std::string &next) {
    if (!o.empty() && !next.empty() && std::isalpha(static_cast<unsigned char>(next[0]))) {
        size_t i = o.size();
        while (i > 0 && std::isalpha(static_cast<unsigned char>(o[i - 1]))) --i;
        if (i > 0 && i < o.size() && o[i - 1] == '\\') o += ' ';
    }
    o += next;
}

std::string OmmlTex(const xml::xml_node &n);
std::string OmmlKids(const xml::xml_node &n) {
    std::string o;
    for (const xml::xml_node &k : n.children()) {
        const std::string l = Local(k);
        if (l.size() > 2 && l.compare(l.size() - 2, 2, "Pr") == 0) continue;  // properties
        Join(o, OmmlTex(k));
    }
    return o;
}
std::string OmmlArg(const xml::xml_node &n, const char *child) { return OmmlKids(n.child(child)); }
std::string OmmlVal(const xml::xml_node &pr, const char *child, const std::string &fallback) {
    const xml::xml_node c = pr.child(child);
    if (!c) return fallback;
    return c.attribute("m:val").as_string();
}
std::string OmmlTex(const xml::xml_node &n) {
    const std::string l = Local(n);
    if (n.type() == xml::node_pcdata) return "";
    if (l == "t") return TexSymbols(n.text().get());
    if (l == "sSup") return OmmlArg(n, "m:e") + "^{" + OmmlArg(n, "m:sup") + "}";
    if (l == "sSub") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:sub") + "}";
    if (l == "sSubSup") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:sub") + "}^{" + OmmlArg(n, "m:sup") + "}";
    if (l == "sPre") return "{}_{" + OmmlArg(n, "m:sub") + "}^{" + OmmlArg(n, "m:sup") + "}" + OmmlArg(n, "m:e");
    if (l == "f") return "\\frac{" + OmmlArg(n, "m:num") + "}{" + OmmlArg(n, "m:den") + "}";
    if (l == "rad") {
        const std::string deg = OmmlArg(n, "m:deg");
        return deg.empty() ? "\\sqrt{" + OmmlArg(n, "m:e") + "}" : "\\sqrt[" + deg + "]{" + OmmlArg(n, "m:e") + "}";
    }
    if (l == "d") {
        const xml::xml_node pr = n.child("m:dPr");
        const auto delim = [](const std::string &c) {
            return c == "{" ? std::string("\\{") : c == "}" ? std::string("\\}") : TexSymbols(c);
        };
        std::string o = delim(OmmlVal(pr, "m:begChr", "("));
        const std::string sep = OmmlVal(pr, "m:sepChr", "|");
        bool first = true;
        for (const xml::xml_node &e : n.children("m:e")) {
            if (!first) o += TexSymbols(sep);
            o += OmmlKids(e);
            first = false;
        }
        return o + delim(OmmlVal(pr, "m:endChr", ")"));
    }
    if (l == "nary") {
        const xml::xml_node pr = n.child("m:naryPr");
        std::string o = TexSymbols(OmmlVal(pr, "m:chr", "∫"));
        const std::string sub = OmmlArg(n, "m:sub"), sup = OmmlArg(n, "m:sup");
        if (!sub.empty()) o += "_{" + sub + "}";
        if (!sup.empty()) o += "^{" + sup + "}";
        return o + " " + OmmlArg(n, "m:e");
    }
    if (l == "func") return OmmlArg(n, "m:fName") + " " + OmmlArg(n, "m:e");
    if (l == "acc") {
        const std::string chr = OmmlVal(n.child("m:accPr"), "m:chr", "̂");
        const std::string cmd = chr == "̇" ? "\\dot" : chr == "̈" ? "\\ddot" : chr == "̃" ? "\\tilde" : chr == "⃗" ? "\\vec" : chr == "̄" || chr == "¯" ? "\\bar" : "\\hat";
        return cmd + "{" + OmmlArg(n, "m:e") + "}";
    }
    if (l == "bar") return "\\overline{" + OmmlArg(n, "m:e") + "}";
    if (l == "limLow") return OmmlArg(n, "m:e") + "_{" + OmmlArg(n, "m:lim") + "}";
    if (l == "limUpp") return OmmlArg(n, "m:e") + "^{" + OmmlArg(n, "m:lim") + "}";
    if (l == "m") {
        std::string o = "\\begin{matrix}";
        bool first_row = true;
        for (const xml::xml_node &r : n.children("m:mr")) {
            o += first_row ? " " : " \\\\ ";
            bool first = true;
            for (const xml::xml_node &e : r.children("m:e")) {
                o += (first ? "" : " & ") + OmmlKids(e);
                first = false;
            }
            first_row = false;
        }
        return o + " \\end{matrix}";
    }
    if (l == "eqArr") {
        std::string o;
        for (const xml::xml_node &e : n.children("m:e")) o += (o.empty() ? "" : " \\\\ ") + OmmlKids(e);
        return "\\begin{aligned}" + o + "\\end{aligned}";
    }
    return OmmlKids(n);
}

std::string MathmlTex(const xml::xml_node &n);
std::string MathmlKids(const xml::xml_node &n) {
    std::string o;
    for (const xml::xml_node &k : n.children())
        if (k.type() == xml::node_element) Join(o, MathmlTex(k));
    return o;
}
std::vector<xml::xml_node> Elements(const xml::xml_node &n) {
    std::vector<xml::xml_node> v;
    for (const xml::xml_node &k : n.children())
        if (k.type() == xml::node_element) v.push_back(k);
    return v;
}
std::string MathmlTex(const xml::xml_node &n) {
    const std::string l = Local(n);
    const std::vector<xml::xml_node> a = Elements(n);
    auto arg = [&](size_t i) { return i < a.size() ? MathmlTex(a[i]) : std::string(); };
    if (l == "semantics") {
        for (const xml::xml_node &k : a)
            if (Local(k) == "annotation" && std::string(k.attribute("encoding").as_string()).find("tex") != std::string::npos)
                return Trim(k.text().get());
        return arg(0);
    }
    if (l == "annotation" || l == "annotation-xml") return "";
    if (l == "mi" || l == "mn" || l == "mo") return TexSymbols(Trim(n.text().get()));
    if (l == "mtext") return "\\text{" + std::string(n.text().get()) + "}";
    if (l == "mspace") return "\\ ";
    if (l == "msup") return arg(0) + "^{" + arg(1) + "}";
    if (l == "msub") return arg(0) + "_{" + arg(1) + "}";
    if (l == "msubsup") return arg(0) + "_{" + arg(1) + "}^{" + arg(2) + "}";
    if (l == "mfrac") return "\\frac{" + arg(0) + "}{" + arg(1) + "}";
    if (l == "msqrt") return "\\sqrt{" + MathmlKids(n) + "}";
    if (l == "mroot") return "\\sqrt[" + arg(1) + "]{" + arg(0) + "}";
    if (l == "mover" || l == "munder") {
        const std::string mark = a.size() > 1 ? Trim(a[1].text().get()) : "";
        const bool over = l == "mover";
        if (over && (mark == "^" || mark == "ˆ")) return "\\hat{" + arg(0) + "}";
        if (over && (mark == "¯" || mark == "‾")) return "\\bar{" + arg(0) + "}";
        if (over && mark == "→") return "\\vec{" + arg(0) + "}";
        if (over && mark == "~") return "\\tilde{" + arg(0) + "}";
        if (over && mark == "˙") return "\\dot{" + arg(0) + "}";
        return arg(0) + (over ? "^{" : "_{") + arg(1) + "}";
    }
    if (l == "munderover") return arg(0) + "_{" + arg(1) + "}^{" + arg(2) + "}";
    if (l == "mfenced") {
        const std::string open = n.attribute("open") ? n.attribute("open").as_string() : "(";
        const std::string close = n.attribute("close") ? n.attribute("close").as_string() : ")";
        std::string o;
        for (size_t i = 0; i < a.size(); ++i) o += (i ? "," : "") + MathmlTex(a[i]);
        return open + o + close;
    }
    if (l == "mtable") {
        std::string o = "\\begin{matrix}";
        for (size_t r = 0; r < a.size(); ++r) {
            o += r ? " \\\\ " : " ";
            const std::vector<xml::xml_node> cells = Elements(a[r]);
            for (size_t c = 0; c < cells.size(); ++c) o += (c ? " & " : "") + MathmlKids(cells[c]);
        }
        return o + " \\end{matrix}";
    }
    return MathmlKids(n);
}


}  // namespace

std::string OmmlToTex(const xml::xml_node &n) { return Trim(OmmlTex(n)); }
std::string MathmlToTex(const xml::xml_node &n) { return Trim(MathmlTex(n)); }

}  // namespace officemath
