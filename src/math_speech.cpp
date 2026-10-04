#include "math_speech.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

namespace mathspeech {

namespace {

// --- the formula as a tree --------------------------------------------------

struct Node {
    enum Kind { Char, Command, Group, Environment } kind = Char;
    std::string text;            // Char: the character(s); Command / Environment: the name
    std::vector<Node> children;  // Group: its content; Command: its arguments, each a Group
    std::vector<Node> sub, sup;  // scripts on the node (each a list)
    bool has_sub = false, has_sup = false;
    int primes = 0;
    // Environment: rows of cells.
    std::vector<std::vector<std::vector<Node>>> rows;
};

// How many brace arguments a command takes (and whether an optional [..] first).
int ArgCount(const std::string &name) {
    static const std::map<std::string, int> k = {
        {"frac", 2},      {"tfrac", 2},      {"dfrac", 2},     {"binom", 2},      {"sqrt", 1},       {"hat", 1},
        {"widehat", 1},   {"bar", 1},        {"overline", 1},  {"tilde", 1},      {"widetilde", 1},  {"vec", 1},
        {"dot", 1},       {"ddot", 1},       {"mathbf", 1},    {"boldsymbol", 1}, {"bm", 1},         {"mathrm", 1},
        {"mathit", 1},    {"mathsf", 1},     {"mathtt", 1},    {"mathcal", 1},    {"mathbb", 1},     {"mathfrak", 1},
        {"text", 1},      {"textbf", 1},     {"textit", 1},    {"textrm", 1},     {"operatorname", 1}, {"underbrace", 1},
        {"overbrace", 1}, {"overset", 2},    {"underset", 2},  {"stackrel", 2},   {"underline", 1},  {"boxed", 1},
        {"mbox", 1},      {"pmb", 1},        {"left", 0},      {"right", 0},      {"color", 1},      {"textcolor", 2},
        {"phantom", 1},   {"tag", 1},
    };
    const auto it = k.find(name);
    return it == k.end() ? 0 : it->second;
}

struct Parser {
    const std::string &s;
    size_t i = 0;
    int depth = 0;

    void Blanks() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    // A run of nodes up to a closing brace, an \end, a `&` or `\\` (when
    // `cells`), or the end.
    std::vector<Node> List(bool cells) {
        std::vector<Node> out;
        while (i < s.size()) {
            Blanks();
            if (i >= s.size()) break;
            const char c = s[i];
            if (c == '}') break;
            if (cells && c == '&') break;
            if (c == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
                if (cells) break;
                i += 2;
                Node sep;
                sep.text = ";";
                out.push_back(sep);
                continue;
            }
            if (s.compare(i, 4, "\\end") == 0 && (i + 4 >= s.size() || !std::isalpha(static_cast<unsigned char>(s[i + 4])))) break;
            if (c == '^' || c == '_') {
                ++i;
                std::vector<Node> script = Argument();
                if (out.empty()) out.emplace_back();  // a script on nothing: on an empty node
                Node &base = out.back();
                if (c == '^') {
                    base.sup.insert(base.sup.end(), script.begin(), script.end());
                    base.has_sup = true;
                } else {
                    base.sub.insert(base.sub.end(), script.begin(), script.end());
                    base.has_sub = true;
                }
                continue;
            }
            if (c == '\'') {
                ++i;
                if (out.empty()) out.emplace_back();
                ++out.back().primes;
                continue;
            }
            out.push_back(One());
        }
        return out;
    }
    // One argument: a brace group's content, or a single node.
    std::vector<Node> Argument() {
        Blanks();
        if (i < s.size() && s[i] == '{') {
            ++i;
            // (Nesting deeper than any formula's is not followed.)
            std::vector<Node> inner;
            if (depth < 60) {
                ++depth;
                inner = List(false);
                --depth;
            }
            if (i < s.size() && s[i] == '}') ++i;
            return inner;
        }
        if (i >= s.size()) return {};
        return {One()};
    }
    std::string Name() {
        // (after a backslash)
        if (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) return std::string(1, s[i++]);
        std::string n;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) n += s[i++];
        return n;
    }
    Node One() {
        Node n;
        const char c = s[i];
        if (c == '{') {
            n.kind = Node::Group;
            n.children = Argument();
            return n;
        }
        if (c == '\\') {
            ++i;
            n.kind = Node::Command;
            n.text = Name();
            if (n.text == "begin") return Env();
            if (n.text == "text" || n.text == "mbox" || n.text == "textrm" || n.text == "textbf" || n.text == "textit") {
                // Text is said as it is written.
                Blanks();
                Node arg;
                arg.kind = Node::Group;
                if (i < s.size() && s[i] == '{') {
                    int level = 0;
                    std::string t;
                    for (; i < s.size(); ++i) {
                        if (s[i] == '{' && level++ == 0) continue;
                        if (s[i] == '}' && --level == 0) {
                            ++i;
                            break;
                        }
                        t += s[i];
                    }
                    Node ch;
                    ch.text = t;
                    arg.children.push_back(ch);
                }
                n.children.push_back(arg);
                return n;
            }
            std::vector<Node> root_index;  // \sqrt[n]{x}'s n
            if (n.text == "sqrt") {
                Blanks();
                if (i < s.size() && s[i] == '[') {
                    const size_t close = s.find(']', i);
                    Node index;
                    index.kind = Node::Group;
                    if (close != std::string::npos) {
                        const std::string inner = s.substr(i + 1, close - i - 1);
                        Parser p{inner};
                        index.children = p.List(false);
                        i = close + 1;
                    }
                    root_index = index.children;
                }
            }
            for (int k = 0; k < ArgCount(n.text); ++k) {
                Node arg;
                arg.kind = Node::Group;
                arg.children = Argument();
                n.children.push_back(arg);
            }
            if (!root_index.empty()) {
                Node index;
                index.kind = Node::Group;
                index.children = root_index;
                n.children.push_back(index);
            }
            return n;
        }
        // A number is one node.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                                    ((s[i] == '.' || s[i] == ',') && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
                                     !n.text.empty())))
                n.text += s[i++];
            return n;
        }
        // (One UTF-8 character.)
        size_t len = 1;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0xF0) len = 4;
        else if (u >= 0xE0) len = 3;
        else if (u >= 0xC0) len = 2;
        n.text = s.substr(i, len);
        i += len;
        return n;
    }
    Node Env() {
        Node n;
        n.kind = Node::Environment;
        Blanks();
        if (i < s.size() && s[i] == '{') {
            const size_t close = s.find('}', i);
            n.text = close == std::string::npos ? "" : s.substr(i + 1, close - i - 1);
            i = close == std::string::npos ? s.size() : close + 1;
        }
        // (An array's column spec.)
        Blanks();
        if (n.text == "array" && i < s.size() && s[i] == '{') {
            const size_t close = s.find('}', i);
            i = close == std::string::npos ? s.size() : close + 1;
        }
        n.rows.emplace_back();
        if (depth >= 60) return n;
        ++depth;
        while (i < s.size()) {
            n.rows.back().push_back(List(true));
            Blanks();
            if (i >= s.size()) break;
            if (s[i] == '&') {
                ++i;
                continue;
            }
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') {
                i += 2;
                n.rows.emplace_back();
                continue;
            }
            if (s.compare(i, 4, "\\end") == 0) {
                const size_t close = s.find('}', i);
                i = close == std::string::npos ? s.size() : close + 1;
                break;
            }
            if (s[i] == '}') ++i;  // (stray)
        }
        --depth;
        // A trailing empty row (after a final \\) says nothing.
        while (!n.rows.empty() && std::all_of(n.rows.back().begin(), n.rows.back().end(), [](const auto &c) { return c.empty(); }))
            n.rows.pop_back();
        return n;
    }
};

// --- saying it --------------------------------------------------------------

const char *Word(const std::string &name) {
    static const std::map<std::string, const char *> k = {
        // Greek
        {"alpha", "alpha"}, {"beta", "beta"}, {"gamma", "gamma"}, {"delta", "delta"}, {"epsilon", "epsilon"},
        {"varepsilon", "epsilon"}, {"zeta", "zeta"}, {"eta", "eta"}, {"theta", "theta"}, {"vartheta", "theta"},
        {"iota", "iota"}, {"kappa", "kappa"}, {"lambda", "lambda"}, {"mu", "mu"}, {"nu", "nu"}, {"xi", "xi"},
        {"pi", "pi"}, {"varpi", "pi"}, {"rho", "rho"}, {"varrho", "rho"}, {"sigma", "sigma"}, {"varsigma", "sigma"},
        {"tau", "tau"}, {"upsilon", "upsilon"}, {"phi", "phi"}, {"varphi", "phi"}, {"chi", "chi"}, {"psi", "psi"},
        {"omega", "omega"}, {"Gamma", "capital gamma"}, {"Delta", "capital delta"}, {"Theta", "capital theta"},
        {"Lambda", "capital lambda"}, {"Xi", "capital xi"}, {"Pi", "capital pi"}, {"Sigma", "capital sigma"},
        {"Upsilon", "capital upsilon"}, {"Phi", "capital phi"}, {"Psi", "capital psi"}, {"Omega", "capital omega"},
        {"ell", "l"}, {"hbar", "h bar"}, {"imath", "i"}, {"jmath", "j"},
        // relations
        {"le", "is less than or equal to"}, {"leq", "is less than or equal to"}, {"ge", "is greater than or equal to"},
        {"geq", "is greater than or equal to"}, {"ne", "is not equal to"}, {"neq", "is not equal to"},
        {"approx", "is approximately"}, {"sim", "is distributed as"}, {"simeq", "is approximately"},
        {"equiv", "is equivalent to"}, {"propto", "is proportional to"}, {"in", "in"}, {"notin", "not in"},
        {"subset", "is a subset of"}, {"subseteq", "is a subset of"}, {"perp", "is perpendicular to"},
        {"parallel", "is parallel to"}, {"mid", "given"}, {"ll", "is much less than"}, {"gg", "is much greater than"},
        // arrows
        {"to", "to"}, {"rightarrow", "to"}, {"longrightarrow", "to"}, {"mapsto", "maps to"}, {"leftarrow", "from"},
        {"Rightarrow", "implies"}, {"Longrightarrow", "implies"}, {"implies", "implies"},
        {"Leftrightarrow", "if and only if"}, {"Longleftrightarrow", "if and only if"}, {"iff", "if and only if"},
        // operators
        {"cdot", "times"}, {"times", "times"}, {"div", "divided by"}, {"pm", "plus or minus"}, {"mp", "minus or plus"},
        {"ast", "star"}, {"star", "star"}, {"circ", "composed with"}, {"oplus", "direct sum"}, {"otimes", "tensor"},
        {"cup", "union"}, {"cap", "intersection"}, {"setminus", "minus"}, {"land", "and"}, {"lor", "or"},
        {"neg", "not"}, {"forall", "for all"}, {"exists", "there exists"},
        // symbols
        {"infty", "infinity"}, {"partial", "partial"}, {"nabla", "del"}, {"emptyset", "the empty set"},
        {"dots", "and so on"}, {"ldots", "and so on"}, {"cdots", "and so on"}, {"vdots", "and so on"},
        {"ddots", "and so on"}, {"top", "transpose"}, {"prime", "prime"}, {"dagger", "dagger"},
        {"langle", "the inner product of"}, {"rangle", ""}, {"lVert", "norm"}, {"rVert", ""},
        {"lvert", "bar"}, {"rvert", ""}, {"%", "percent"}, {"$", "dollars"}, {"&", "and"}, {"#", "number"},
        {"_", "underscore"}, {"{", "the set"}, {"}", ""},
        // functions
        {"log", "log"}, {"ln", "natural log"}, {"exp", "exp"}, {"sin", "sine"}, {"cos", "cosine"}, {"tan", "tangent"},
        {"min", "the minimum"}, {"max", "the maximum"}, {"arg", "arg"}, {"det", "the determinant of"},
        {"dim", "the dimension of"}, {"lim", "the limit"}, {"sup", "the supremum"}, {"inf", "the infimum"},
        {"Pr", "the probability of"}, {"tr", "trace"},
    };
    const auto it = k.find(name);
    return it == k.end() ? nullptr : it->second;
}

bool IsGreek(const std::string &n) {
    static const char *k[] = {"alpha", "beta", "gamma", "delta", "epsilon", "varepsilon", "zeta", "eta", "theta", "vartheta",
                              "iota", "kappa", "lambda", "mu", "nu", "xi", "pi", "varpi", "rho", "varrho", "sigma", "varsigma",
                              "tau", "upsilon", "phi", "varphi", "chi", "psi", "omega", "Gamma", "Delta", "Theta", "Lambda",
                              "Xi", "Pi", "Sigma", "Upsilon", "Phi", "Psi", "Omega", "ell"};
    return std::any_of(std::begin(k), std::end(k), [&](const char *x) { return n == x; });
}

// Commands that say nothing: spacing, sizes, styles.
bool Silent(const std::string &n) {
    static const char *k[] = {",",  ";",  "!",  ":",  " ",  "left",  "right",  "big",  "Big",  "bigg",  "Bigg",
                              "bigl", "bigr", "Bigl", "Bigr", "biggl", "biggr", "displaystyle", "textstyle", "scriptstyle",
                              "limits", "nolimits", "nonumber", "notag", "quad", "hfill", "hspace", "vspace", "phantom",
                              "color", "tag", "label", "mathstrut", "strut", "middle"};
    return std::any_of(std::begin(k), std::end(k), [&](const char *x) { return n == x; });
}

bool IsFont(const std::string &n) {
    static const char *k[] = {"mathbf", "boldsymbol", "bm", "mathrm", "mathit", "mathsf", "mathtt", "mathcal", "mathbb",
                              "mathfrak", "pmb", "underline", "boxed", "mbox", "text", "textbf", "textit", "textrm"};
    return std::any_of(std::begin(k), std::end(k), [&](const char *x) { return n == x; });
}

struct Speaker {
    std::string out;

    void Say(const std::string &words) {
        if (words.empty()) return;
        const bool punct = words[0] == ',' || words[0] == ';' || words[0] == '.' || words[0] == ':';
        if (!out.empty() && out.back() != ' ' && !punct) out += ' ';
        // (No two commas in a row, and none at the start.)
        if (punct) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (out.empty() || out.back() == ',' || out.back() == ';' || out.back() == ':') return;
        }
        out += words;
    }
    static std::string Text(const std::vector<Node> &list) {
        // (Brackets inside brackets inside brackets: not followed for ever.)
        static thread_local int nesting = 0;
        if (nesting > 80) return "";
        ++nesting;
        Speaker s;
        s.List(list);
        --nesting;
        return s.Done();
    }
    std::string Done() {
        while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == ';')) out.pop_back();
        return out;
    }
    static int WordCount(const std::string &t) {
        int n = 0;
        bool in = false;
        for (char c : t) {
            const bool sp = c == ' ';
            if (!sp && !in) ++n;
            in = !sp;
        }
        return n;
    }
    // The plain characters of a list ("-1", "(t)"), "" if it holds anything else.
    static std::string Plain(const std::vector<Node> &list) {
        std::string t;
        for (const Node &n : list) {
            if (n.has_sub || n.has_sup || n.primes) return "";
            if (n.kind == Node::Char) t += n.text;
            else if (n.kind == Node::Command && n.children.empty()) t += "\\" + n.text;
            else if (n.kind == Node::Command && IsFont(n.text) && n.children.size() == 1) t += Plain(n.children[0].children);
            else return "";
        }
        return t;
    }
    // Whether what was just said can be applied to something in brackets: f(x), E[Y], \log(...).
    bool after_function = false;
    // ... or a letter of any kind (then `a(\phi)` is "a of phi" when the
    // brackets hold no sum or relation), or a number (`8(0.5)`: a product).
    bool after_letter = false, after_number = false;

    void Scripts(const Node &n, bool big_operator) {
        if (big_operator) {
            // "the sum from i equals 1 to n of" / "the sum over i of"
            if (n.has_sub && n.has_sup) Say("from " + Text(n.sub) + " to " + Text(n.sup));
            else if (n.has_sub) Say("over " + Text(n.sub));
            else if (n.has_sup) Say("to " + Text(n.sup));
            Say("of");
            return;
        }
        if (n.has_sub) {
            const std::string plain = Plain(n.sub);
            const std::string t = plain == "\\max" ? "max" : plain == "\\min" ? "min" : Text(n.sub);
            // A plain index is just said after its letter: "x i", "theta 0".
            Say(WordCount(t) <= 1 ? t : "sub " + t + ",");
        }
        for (int p = 0; p < n.primes; ++p) Say("prime");
        if (n.has_sup) {
            const std::string plain = Plain(n.sup);
            if (plain == "2") Say("squared");
            else if (plain == "3") Say("cubed");
            else if (plain == "\\top" || plain == "T" || plain == "\\intercal") Say("transpose");
            else if (plain == "-1") Say("inverse");
            else if (plain == "-\\top" || plain == "-T") Say("inverse transpose");
            else if (plain == "*" || plain == "\\ast" || plain == "\\star") Say("star");
            else if (plain == "*\\top" || plain == "\\ast\\top") Say("star transpose");
            else if (plain == "\\prime") Say("prime");
            else if (plain == "\\circ") Say("degrees");
            else if (plain == "1/2") Say("to the one half");
            else if (plain == "-1/2") Say("to the minus one half");
            else if (plain.size() >= 2 && plain.front() == '(' && plain.back() == ')')
                Say("superscript " + Text(std::vector<Node>(n.sup.begin() + 1, n.sup.end() - 1)) + ",");
            else {
                const std::string t = Text(n.sup);
                if (!t.empty()) Say("to the " + t + (WordCount(t) > 1 ? "," : ""));
            }
        }
    }

    // The nodes from an opening bracket at list[at] to its match: says
    // them, and returns the index of the closing one (or the last node).
    size_t Bracket(const std::vector<Node> &list, size_t at, const std::string &open, const std::string &close) {
        int level = 0;
        size_t end = at;
        for (size_t k = at; k < list.size(); ++k) {
            if (list[k].kind == Node::Char && list[k].text == open) ++level;
            else if (list[k].kind == Node::Char && list[k].text == close && --level == 0) {
                end = k;
                break;
            }
            end = k;
        }
        const bool closed = end > at && list[end].kind == Node::Char && list[end].text == close;
        const std::vector<Node> inner(list.begin() + static_cast<long>(at) + 1, list.begin() + static_cast<long>(closed ? end : end + 1));
        const std::string t = Text(inner);
        bool top_operator = false;
        int top_comma = 0;
        int depth = 0;
        for (const Node &n : inner) {
            if (n.kind == Node::Command && depth == 0 && Word(n.text) && !IsGreek(n.text) && n.text != "top" && n.text != "prime" &&
                n.text != "log" && n.text != "exp" && n.text != "partial" && n.text != "nabla" && n.text != "infty")
                top_operator = true;
            if (n.kind != Node::Char) continue;
            if (n.text == "(" || n.text == "[") ++depth;
            else if (n.text == ")" || n.text == "]") --depth;
            else if (n.text == "," && depth == 0) ++top_comma;
            else if (depth == 0 && (n.text == "+" || n.text == "-" || n.text == "=" || n.text == "<" || n.text == ">")) top_operator = true;
        }
        const bool applied = after_function || (after_letter && !top_operator);
        // ("the trace of" already says "of".)
        const bool said_of = out.size() >= 3 && out.compare(out.size() - 3, 3, " of") == 0;
        if (applied) Say((said_of ? "" : "of ") + t + (WordCount(t) > 1 ? "," : ""));
        else if (after_number && WordCount(t) <= 1) Say("times " + t);
        else if (WordCount(t) <= 1) Say(t);
        else if (top_comma) Say(std::string(top_comma == 1 ? "the pair " : "the tuple ") + t + ",");
        else Say("the quantity " + t + ",");
        after_function = after_letter = after_number = false;
        // The bracket's own scripts: "(X transpose X) inverse".
        if (closed) Scripts(list[end], false);
        return end;
    }
    // `\| ... \|` and `| ... |` around something: its norm, its absolute value.
    bool Bars(const std::vector<Node> &list, size_t at, size_t *end) {
        const Node &open = list[at];
        const bool norm = open.kind == Node::Command && open.text == "|";
        if (!norm && !(open.kind == Node::Char && open.text == "|")) return false;
        if (open.has_sub || open.has_sup) return false;
        for (size_t k = at + 1; k < list.size(); ++k) {
            const Node &n = list[k];
            const bool same = norm ? (n.kind == Node::Command && n.text == "|") : (n.kind == Node::Char && n.text == "|");
            if (!same) continue;
            if (k == at + 1) return false;
            const std::vector<Node> inner(list.begin() + static_cast<long>(at) + 1, list.begin() + static_cast<long>(k));
            Say(std::string(norm ? "the norm of " : "the absolute value of ") + Text(inner) + ",");
            Scripts(n, false);
            *end = k;
            return true;
        }
        return false;
    }

    void List(const std::vector<Node> &list) {
        for (size_t k = 0; k < list.size(); ++k) {
            const Node &n = list[k];
            if (n.kind == Node::Char && (n.text == "(" || n.text == "[")) {
                k = Bracket(list, k, n.text, n.text == "(" ? ")" : "]");
                continue;
            }
            size_t end = k;
            if (Bars(list, k, &end)) {
                k = end;
                after_function = after_letter = after_number = false;
                continue;
            }
            One(n);
        }
    }

    void One(const Node &n) {
        bool function = false;
        bool letter = false, number = false;
        switch (n.kind) {
            case Node::Char: {
                const std::string &c = n.text;
                if (c.empty()) break;
                if (c == "=") Say("equals");
                else if (c == "+") Say("plus");
                else if (c == "-") Say("minus");
                else if (c == "<") Say("is less than");
                else if (c == ">") Say("is greater than");
                else if (c == "/") Say("over");
                else if (c == "*") Say("times");
                else if (c == "!") Say("factorial");
                else if (c == "|") Say("given");
                else if (c == ":") Say(":");
                else if (c == ",") Say(",");
                else if (c == ";") Say(";");
                else if (c == ".") Say(".");
                else if (c == ")" || c == "]" || c == "~" || c == "&") break;
                else {
                    Say(c);
                    function = c.size() == 1 && std::strchr("fghFGLNEPVQbacsq", c[0]) != nullptr;
                    number = std::isdigit(static_cast<unsigned char>(c[0])) != 0;
                    letter = !number && (c.size() > 1 || std::isalpha(static_cast<unsigned char>(c[0])));
                }
                break;
            }
            case Node::Group: {
                const std::string t = Text(n.children);
                Say(t);
                break;
            }
            case Node::Environment: Environment(n); break;
            case Node::Command:
                function = Command(n);
                // (A Greek letter, a letter in some font, a letter with a hat.)
                letter = !Silent(n.text) && (IsFont(n.text) || n.text == "hat" || n.text == "bar" || n.text == "tilde" ||
                                             n.text == "operatorname" || (Word(n.text) && IsGreek(n.text)));
                if (Silent(n.text)) letter = after_letter, number = after_number;
                break;
        }
        const bool big = n.kind == Node::Command && (n.text == "sum" || n.text == "prod" || n.text == "int" || n.text == "min" ||
                                                     n.text == "max" || n.text == "lim" || n.text == "sup" || n.text == "inf" ||
                                                     n.text == "bigcup" || n.text == "bigcap" || n.text == "arg");
        if (n.kind == Node::Command && (n.text == "underbrace" || n.text == "overbrace")) {
            // "x, which is y,": the brace's label says what the piece is.
            const std::string label = Text(n.text == "underbrace" ? n.sub : n.sup);
            if (!label.empty()) Say(", which is " + label + ",");
            else Say(",");
            Node rest = n;
            (n.text == "underbrace" ? rest.has_sub : rest.has_sup) = false;
            Scripts(rest, false);
        } else if (big && (n.has_sub || n.has_sup)) {
            Scripts(n, true);
        } else {
            Scripts(n, false);
        }
        after_function = function && !n.has_sup;
        after_letter = letter && !n.has_sup;
        after_number = number && !n.has_sup && !n.has_sub;
    }

    // Returns whether the command names something applied to what follows.
    bool Command(const Node &n) {
        const std::string &c = n.text;
        auto arg = [&](size_t k) { return k < n.children.size() ? Text(n.children[k].children) : std::string(); };
        if (c == "frac" || c == "tfrac" || c == "dfrac") {
            const std::string a = arg(0), b = arg(1);
            if (WordCount(a) <= 1 && WordCount(b) <= 1) Say(a + " over " + b);
            else Say("the fraction " + a + ", over " + b + ",");
            return false;
        }
        if (c == "binom") {
            Say(arg(0) + " choose " + arg(1));
            return false;
        }
        if (c == "sqrt") {
            const std::string index = arg(1);
            Say((index.empty() ? "the square root of " : "the " + index + "-th root of ") + arg(0) + (WordCount(arg(0)) > 1 ? "," : ""));
            return false;
        }
        if (c == "hat" || c == "widehat") return Say(arg(0) + " hat"), false;
        if (c == "bar" || c == "overline") return Say(arg(0) + " bar"), false;
        if (c == "tilde" || c == "widetilde") return Say(arg(0) + " tilde"), false;
        if (c == "dot") return Say(arg(0) + " dot"), false;
        if (c == "ddot") return Say(arg(0) + " double dot"), false;
        if (c == "vec") return Say("vector " + arg(0)), false;
        if (c == "operatorname") {
            const std::string name = n.children.empty() ? "" : Plain(n.children[0].children);
            static const std::map<std::string, const char *> k = {
                {"tr", "the trace of"}, {"diag", "diag"}, {"rank", "the rank of"}, {"Var", "the variance of"},
                {"Cov", "the covariance of"}, {"SD", "the standard deviation of"}, {"logit", "logit"},
                {"Bernoulli", "Bernoulli"}, {"Poisson", "Poisson"}, {"argmin", "the minimizer"}, {"argmax", "the maximizer"}};
            const auto it = k.find(name);
            Say(it != k.end() ? it->second : name.empty() ? arg(0) : name);
            return true;
        }
        if (c == "mathrm" || c == "text" || c == "mbox" || c == "textrm" || c == "textbf" || c == "textit") {
            const std::string t = n.children.empty() ? "" : Plain(n.children[0].children);
            std::string said = t.empty() ? arg(0) : t;
            // (Trim: \text{ known} )
            while (!said.empty() && said.front() == ' ') said.erase(said.begin());
            while (!said.empty() && said.back() == ' ') said.pop_back();
            Say(said);
            // An upright name is a function's (SSE, logit) when brackets follow.
            return c == "mathrm" && !said.empty() && said.find(' ') == std::string::npos;
        }
        if (IsFont(c)) {
            const std::string t = arg(0);
            Say(t);
            const std::string plain = n.children.empty() ? "" : Plain(n.children[0].children);
            return plain.size() == 1 && std::strchr("fghFGLNEPVQbacsq", plain[0]) != nullptr;
        }
        if (c == "underbrace" || c == "overbrace") return Say(arg(0)), false;
        if (c == "overset" || c == "stackrel") {
            // What sits above qualifies what is below: "is distributed as, iid,".
            Say(arg(1) + ", " + arg(0) + ",");
            return false;
        }
        if (c == "underset") return Say(arg(1) + ", " + arg(0) + ","), false;
        if (c == "textcolor") return Say(arg(1)), false;
        if (c == "qquad") return Say(";"), false;
        if (c == "sum") return Say("the sum"), false;
        if (c == "prod") return Say("the product"), false;
        if (c == "int") return Say("the integral"), false;
        if (c == "|") return Say("norm"), false;
        if (Silent(c)) return after_function;  // (spacing between a function and its bracket: \exp\!\left( )
        if (const char *w = Word(c)) {
            Say(w);
            return c == "log" || c == "ln" || c == "exp" || c == "sin" || c == "cos" || c == "tan" || c == "ell" || c == "Pr" ||
                   c == "det" || c == "dim" || c == "tr";
        }
        Say(c);  // (not a command this knows: its name)
        return false;
    }

    void Environment(const Node &n) {
        const std::string &e = n.text;
        const bool matrix = e.find("matrix") != std::string::npos || e == "array";
        if (!matrix) {
            // aligned, cases, gathered ...: its lines, one after another.
            for (size_t r = 0; r < n.rows.size(); ++r) {
                for (const auto &cell : n.rows[r]) List(cell);
                if (r + 1 < n.rows.size()) Say(";");
            }
            return;
        }
        size_t cols = 0;
        for (const auto &row : n.rows) cols = std::max(cols, row.size());
        if (cols <= 1) {
            Say("the vector");
            for (size_t r = 0; r < n.rows.size(); ++r) {
                Say(n.rows[r].empty() ? "" : Text(n.rows[r][0]));
                if (r + 1 < n.rows.size()) Say(",");
            }
            Say(",");
            return;
        }
        if (n.rows.size() == 1) {
            Say("the row vector");
            for (size_t c = 0; c < n.rows[0].size(); ++c) {
                Say(Text(n.rows[0][c]));
                if (c + 1 < n.rows[0].size()) Say(",");
            }
            Say(",");
            return;
        }
        Say("the matrix with rows");
        for (size_t r = 0; r < n.rows.size(); ++r) {
            for (size_t c = 0; c < n.rows[r].size(); ++c) {
                Say(Text(n.rows[r][c]));
                if (c + 1 < n.rows[r].size()) Say(",");
            }
            if (r + 1 < n.rows.size()) Say(";");
        }
        Say(",");
    }
};

}  // namespace

std::string Speak(const std::string &tex) {
    Parser p{tex};
    std::vector<Node> list;
    // (A stray closing brace must not stop the reading.)
    while (p.i < tex.size()) {
        std::vector<Node> part = p.List(false);
        list.insert(list.end(), part.begin(), part.end());
        if (p.i < tex.size()) ++p.i;
    }
    Speaker s;
    s.List(list);
    return s.Done();
}

}  // namespace mathspeech
