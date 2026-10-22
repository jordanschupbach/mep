// The analysis half of mep's own Common Lisp language server: an index of
// what a document binds and uses -- built by walking lisp_reader.h's tree
// with the standard's special operators and binding macros understood --
// then the diagnostics and editor features read off it. See lisp_lsp.h
// for the contract and the scope notes.

#include "lisp_lsp.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lisp_format.h"
#include "lisp_lsp_cl_names.h"

// --- The COMMON-LISP table ---------------------------------------------

const LispClDecl *LispLspFindCl(const std::string &upper_name) {
    size_t lo = 0;
    size_t hi = kLispClDeclCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = std::strcmp(kLispClDecls[mid].name, upper_name.c_str());
        if (c == 0) return &kLispClDecls[mid];
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return nullptr;
}

bool LispLspClHasKind(const LispClDecl &decl, const char *kind) {
    const std::string kinds = decl.kinds;
    const std::string want = kind;
    size_t start = 0;
    while (start <= kinds.size()) {
        size_t comma = kinds.find(',', start);
        if (comma == std::string::npos) comma = kinds.size();
        if (kinds.compare(start, comma - start, want) == 0 && comma - start == want.size()) return true;
        start = comma + 1;
    }
    return false;
}

namespace {

// --- Small helpers ----------------------------------------------------

std::string Lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string Upper(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

bool StartsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

/** @brief Levenshtein distance, capped: anything past `limit` is reported as `limit + 1`. */
int EditDistance(const std::string &a, const std::string &b, int limit) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n > m + static_cast<size_t>(limit) || m > n + static_cast<size_t>(limit)) return limit + 1;
    std::vector<int> prev(m + 1);
    std::vector<int> cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; i++) {
        cur[0] = static_cast<int>(i);
        int row_best = cur[0];
        for (size_t j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            row_best = std::min(row_best, cur[j]);
        }
        if (row_best > limit) return limit + 1;
        prev = cur;
    }
    return prev[m];
}

/** @brief `*name*`: the naming convention for special variables. */
bool IsEarmuffed(const std::string &name) { return name.size() > 2 && name.front() == '*' && name.back() == '*'; }

/** @brief `+name+`: the naming convention for constants. */
bool IsPlusNamed(const std::string &name) { return name.size() > 2 && name.front() == '+' && name.back() == '+'; }

/** @brief Reports whether a CL symbol names an operator (function, macro, special operator, generic function). */
bool ClIsOperator(const LispClDecl *d) {
    // CALL-NEXT-METHOD and friends are local functions every method body
    // has; SBCL leaves the global symbols unbound.
    if (d != nullptr && (std::strcmp(d->name, "CALL-NEXT-METHOD") == 0 || std::strcmp(d->name, "NEXT-METHOD-P") == 0 ||
                         std::strcmp(d->name, "MAKE-METHOD") == 0)) {
        return true;
    }
    return d != nullptr && (LispLspClHasKind(*d, "function") || LispLspClHasKind(*d, "macro") ||
                            LispLspClHasKind(*d, "special-operator") || LispLspClHasKind(*d, "generic-function"));
}

/** @brief Reports whether a CL symbol is a variable or constant. */
bool ClIsVariable(const LispClDecl *d) {
    return d != nullptr && (LispLspClHasKind(*d, "variable") || LispLspClHasKind(*d, "constant"));
}

/** @brief The range of a symbol's name part (no package prefix). */
LispRange NameRange(const LispNode &n) {
    LispRange r = n.range;
    if (n.kind == LispNodeKind::Symbol) r.col = n.name_col;
    return r;
}

/** @brief Position order. */
bool PosLess(int l1, int c1, int l2, int c2) { return l1 < l2 || (l1 == l2 && c1 < c2); }

// --- The document index ------------------------------------------------

enum class Ns { Var, Func, Block, Tag, Type, Data };

enum class BK {
    Global,       // defvar / defparameter / declaim special
    Constant,     // defconstant
    GlobalSymbolMacro,
    Function,     // defun
    Macro,        // defmacro, define-modify-macro
    Generic,      // defgeneric, or the implicit generic of a defmethod
    Accessor,     // a defclass/define-condition reader, writer or accessor
    StructFn,     // a defstruct constructor, copier, predicate or accessor
    SetfFn,       // (defun (setf foo) ...), defsetf
    TypeName,     // defclass, defstruct, define-condition, deftype
    Lexical,      // let, let*, multiple-value-bind, prog, ...
    Param,        // a lambda-list parameter
    SymbolMacro,  // symbol-macrolet, with-slots, with-accessors
    LocalFunction,
    LocalMacro,
    BlockName,
    TagName,
    LoopVar,
};

struct Binding {
    std::string name;  // canonical; "(SETF FOO)" for a setf function
    Ns ns = Ns::Var;
    BK kind = BK::Lexical;
    int node = -1;     // the naming symbol
    LispRange range;   // its name part
    LispRange def_range;  // the whole defining form
    int scope = 0;
    int reads = 0;          // every use, checked code or not
    int checked_reads = 0;  // uses in code the name checks run on
    int writes = 0;
    bool check_unused = false;
    bool ignored = false;
    bool ignorable = false;
    bool special = false;
    bool declared_special = false;  // special by a defvar, a CL variable or a declaration -- not by name alone
    bool conditional = false;  // defined under a #+/#- that may not hold
    bool has_shape = false;
    bool check_keys = false;  // unknown keywords at a call are reportable (defun/flet: not generic)
    LispLambdaShape shape;
    std::string lambda_text;  // the lambda list as written, outer parens included
    std::string doc;
    std::string origin;  // "let", "defun", "parameter of FOO", ...
    int def_count = 1;   // definitions of this global name in the file, of any kind
    std::string pkg;     // the package current at a global definition (the last in-package above it)
};

struct Scope {
    int parent = -1;
    LispRange range;
    std::vector<int> bindings;
};

struct Occ {
    int node = -1;
    std::string name;
    std::string package;
    Ns ns = Ns::Var;
    LispRange range;  // the name part
    int binding = -1;
    bool write = false;
    bool call = false;
    bool def = false;
    bool checked = false;
    bool in_template = false;  // a Data occurrence inside a backquote template
    int scope = 0;
};

struct UnknownCall {
    std::string name;
    LispRange range;
};

struct Index {
    LispReadResult read;
    std::vector<Scope> scopes;
    std::vector<Binding> bindings;
    std::vector<Occ> occs;
    std::vector<LispLspDiagnostic> diags;
    std::map<int, int> def_binding;  // defining form node -> its binding
    std::vector<UnknownCall> unknown_calls;
    std::vector<UnknownCall> unknown_setqs;
    std::vector<LispRange> read_evals;  // `#.(...)` computations in code position
    std::vector<LispRange> capturing_calls;  // calls of with-/do- macros defined elsewhere
    std::set<std::string> extra_known;  // :import-from / :shadow names, SBCL's cl-user extras
    bool names_known = false;           // see lisp_lsp.h: the file's package uses only COMMON-LISP
    // The file never leaves CL-USER, which also :uses whatever extension
    // packages the implementation has (SB-EXT, CCL, EXT, ...); there only
    // a near-miss of a known name is reported.
    bool cl_user = false;
    bool custom_readtable = false;
};

// SBCL's CL-USER also uses SB-EXT and friends, so a script in cl-user can
// call these unqualified. Listed rather than guessed so a script that
// calls `(exit)` is not told `exit` is undefined.
const char *const kSbclUserNames[] = {
    "EXIT", "QUIT", "GC", "POSIX-GETENV", "POSIX-ENVIRON", "RUN-PROGRAM", "PROCESS-OUTPUT", "PROCESS-EXIT-CODE",
    "PROCESS-INPUT", "PROCESS-WAIT", "PROCESS-CLOSE", "SAVE-LISP-AND-DIE", "WITHOUT-PACKAGE-LOCKS",
    "DEFGLOBAL", "DEFINE-HASH-TABLE-TEST", "TRULY-THE", "ADD-PACKAGE-LOCAL-NICKNAME", "STRING-TO-OCTETS",
    "OCTETS-TO-STRING", "NATIVE-NAMESTRING", "PARSE-NATIVE-NAMESTRING", "*POSIX-ARGV*", "*RUNTIME-PATHNAME*",
    "*CORE-PATHNAME*", "DISABLE-DEBUGGER", "ENABLE-DEBUGGER", "MUFFLE-CONDITIONS", "DEFINE-ALIEN-ROUTINE",
    "WITH-ALIEN", "ALIEN-FUNCALL", "LOAD-SHARED-OBJECT", "BACKTRACE", "PRINT-BACKTRACE", "ATOMIC-INCF",
    "COMPARE-AND-SWAP", "WORD", "SEQ",
};

// --- Feature expressions -----------------------------------------------

// The features the analysis assumes hold. Conservative: the common
// denominator of SBCL on 64-bit Linux, which is what the generated name
// table describes, plus what every implementation shares.
const char *const kTrueFeatures[] = {
    "COMMON-LISP", "ANSI-CL", "SBCL", "X86-64", "64-BIT", "UNIX", "LINUX", "ELF", "LITTLE-ENDIAN",
    "SB-THREAD", "SB-UNICODE", "IEEE-FLOATING-POINT", "GENCGC", "SB-PACKAGE-LOCKS", "ASDF", "ASDF2", "ASDF3",
    "ASDF3.1", "ASDF3.2", "ASDF3.3", "ASDF-UNICODE", "OS-UNIX", "SB-DOC", "SB-LDB", "SB-CORE-COMPRESSION",
};

// Features that are false on that platform but name a real one: code
// under `#+ccl` is live code, just not for this image.
const char *const kOtherFeatures[] = {
    "CCL", "CLOZURE", "CLOZURE-COMMON-LISP", "OPENMCL", "ECL", "CLISP", "ALLEGRO", "EXCL", "FRANZ-INC",
    "LISPWORKS", "LISPWORKS-PERSONAL-EDITION", "ABCL", "ARMEDBEAR", "CMU", "CMUCL", "CMU19", "CMU20", "SCL",
    "MKCL", "CLASP", "GENERA", "MCL", "CORMANLISP", "XCL", "MEZZANO", "WINDOWS", "WIN32", "MSWINDOWS",
    "OS-WINDOWS", "DARWIN", "MACOS", "MACOSX", "OS-MACOSX", "X86", "PPC", "ARM", "ARM64", "32-BIT",
    "BIG-ENDIAN", "LISPWORKS6", "LISPWORKS7", "LISPWORKS8", "MSVC", "ANDROID", "FREEBSD", "OPENBSD", "NETBSD",
};

enum class Tri { True, False, Unknown };

struct FeatureValue {
    Tri value = Tri::Unknown;
    bool constant = false;  // independent of the feature set: (or), (and), nil
};

FeatureValue EvalFeature(const LispReadResult &r, int node) {
    const LispNode &n = r.at(node);
    FeatureValue out;
    if (n.kind == LispNodeKind::Symbol) {
        if (n.name == "NIL") return {Tri::False, true};  // `#+nil`: the traditional "commented out"
        for (const char *f : kTrueFeatures) {
            if (n.name == f) return {Tri::True, false};
        }
        for (const char *f : kOtherFeatures) {
            if (n.name == f) return {Tri::False, false};
        }
        return out;
    }
    if (n.kind != LispNodeKind::List || n.kids.empty()) {
        if (n.kind == LispNodeKind::List) return {Tri::False, true};  // `()` is nil
        return out;
    }
    const LispNode &op = r.at(n.kids[0]);
    const std::string opname = op.kind == LispNodeKind::Symbol ? op.name : std::string();
    if (opname == "NOT") {
        if (n.kids.size() != 2) return out;
        FeatureValue v = EvalFeature(r, n.kids[1]);
        if (v.value == Tri::True) v.value = Tri::False;
        else if (v.value == Tri::False) v.value = Tri::True;
        return v;
    }
    if (opname != "AND" && opname != "OR") return out;
    const bool is_and = opname == "AND";
    bool all_constant = true;
    bool any_unknown = false;
    for (size_t i = 1; i < n.kids.size(); i++) {
        const FeatureValue v = EvalFeature(r, n.kids[i]);
        if (!v.constant) all_constant = false;
        if (is_and && v.value == Tri::False && v.constant) return {Tri::False, true};
        if (!is_and && v.value == Tri::True && v.constant) return {Tri::True, true};
        if (v.value == Tri::Unknown) any_unknown = true;
        if (is_and && v.value == Tri::False) {
            out.value = Tri::False;
            return out;
        }
        if (!is_and && v.value == Tri::True) {
            out.value = Tri::True;
            return out;
        }
    }
    if (any_unknown) return out;
    return {is_and ? Tri::True : Tri::False, all_constant};
}

enum class FeatureState { Active, Other, Dead };

/** @brief Whether the form of a Feature node (#+/#-) is read on the assumed platform, elsewhere, or never. */
FeatureState FeatureStateOf(const LispReadResult &r, const LispNode &feature) {
    if (feature.kids.empty()) return FeatureState::Dead;
    FeatureValue v = EvalFeature(r, feature.kids[0]);
    if (feature.text == "#-") {
        if (v.value == Tri::True) v.value = Tri::False;
        else if (v.value == Tri::False) v.value = Tri::True;
    }
    if (v.value == Tri::True) return FeatureState::Active;
    if (v.value == Tri::False && v.constant) return FeatureState::Dead;
    return FeatureState::Other;
}

// --- Lambda lists -------------------------------------------------------

/** @brief The keyword name a &key parameter spec accepts: `x`, `(x 1)` and `((:kw x) 1)` forms. */
std::string KeyNameOfSpec(const LispReadResult &r, int spec) {
    const LispNode &n = r.at(spec);
    if (n.kind == LispNodeKind::Symbol) return n.name;
    if (n.kind == LispNodeKind::List && !n.kids.empty()) {
        const LispNode &head = r.at(n.kids[0]);
        if (head.kind == LispNodeKind::Symbol) return head.name;
        if (head.kind == LispNodeKind::List && !head.kids.empty() && r.at(head.kids[0]).kind == LispNodeKind::Symbol) {
            return r.at(head.kids[0]).name;
        }
    }
    return std::string();
}

/** @brief Reads the call shape of a lambda-list node (ordinary, macro or specialized). */
LispLambdaShape ShapeOfNode(const LispReadResult &r, int node) {
    LispLambdaShape shape;
    const LispNode &n = r.at(node);
    if (n.kind != LispNodeKind::List) return shape;
    enum { Req, Opt, Rest, Key, Aux, Skip } state = Req;
    auto prev = Req;
    for (size_t i = 0; i < n.kids.size(); i++) {
        const LispNode &k = r.at(n.kids[i]);
        if (n.dotted && i + 1 == n.kids.size()) {
            shape.rest = true;  // (a b . rest)
            break;
        }
        if (k.kind == LispNodeKind::Symbol && !k.name.empty() && k.name[0] == '&') {
            if (k.name == "&OPTIONAL") state = Opt;
            else if (k.name == "&REST" || k.name == "&BODY") {
                shape.rest = true;
                state = Rest;
            } else if (k.name == "&KEY") {
                shape.key = true;
                state = Key;
            } else if (k.name == "&ALLOW-OTHER-KEYS") shape.allow_other_keys = true;
            else if (k.name == "&AUX") state = Aux;
            else if (k.name == "&WHOLE" || k.name == "&ENVIRONMENT") {
                prev = state;
                state = Skip;
            } else {
                // An implementation's own lambda-list keyword (`&more`):
                // the shape cannot be trusted past it.
                shape.rest = true;
                state = Aux;
            }
            continue;
        }
        switch (state) {
            case Req: shape.required++; break;
            case Opt: shape.optional++; break;
            case Rest: break;
            case Key: {
                const std::string key = KeyNameOfSpec(r, n.kids[i]);
                if (!key.empty()) shape.keys.push_back(key);
                break;
            }
            case Aux: break;
            case Skip: state = prev; break;
        }
    }
    return shape;
}

}  // namespace

std::vector<std::string> LispLspSplitLambdaList(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (in_string) {
            cur += c;
            if (c == '\\' && i + 1 < text.size()) {
                cur += text[++i];
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') in_string = true;
        if (c == '(') depth++;
        if (c == ')') depth--;
        if ((c == ' ' || c == '\t' || c == '\n') && depth <= 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

LispLambdaShape LispLspShapeOfLambdaList(const std::string &text) {
    const LispReadResult r = LispRead({"(" + text + ")"});
    if (r.top.empty()) return LispLambdaShape{};
    return ShapeOfNode(r, r.top[0]);
}

namespace {

// Shapes of the CL table, parsed once on first use.
const LispLambdaShape *ClShape(const LispClDecl *d) {
    static std::map<const LispClDecl *, LispLambdaShape> cache;
    if (d == nullptr) return nullptr;
    auto it = cache.find(d);
    if (it == cache.end()) it = cache.emplace(d, LispLspShapeOfLambdaList(d->lambda_list)).first;
    return &it->second;
}

/** @brief The source text of a node, from the original lines. */
std::string SourceText(const std::vector<std::string> &lines, const LispRange &r) {
    if (r.line < 0 || static_cast<size_t>(r.line) >= lines.size()) return std::string();
    std::string out;
    for (int l = r.line; l <= r.end_line && static_cast<size_t>(l) < lines.size(); l++) {
        const std::string &text = lines[static_cast<size_t>(l)];
        const size_t from = l == r.line ? std::min(text.size(), static_cast<size_t>(r.col)) : 0;
        const size_t to = l == r.end_line ? std::min(text.size(), static_cast<size_t>(r.end_col)) : text.size();
        if (l != r.line) out += ' ';
        if (to > from) {
            std::string piece = text.substr(from, to - from);
            if (l != r.line) {
                const size_t b = piece.find_first_not_of(" \t");
                piece = b == std::string::npos ? std::string() : piece.substr(b);
            }
            out += piece;
        }
    }
    return out;
}

/** @brief The first paragraph of a docstring, whitespace-collapsed. */
std::string FirstParagraph(const std::string &doc) {
    std::string out;
    bool last_space = false;
    int newlines = 0;
    for (char c : doc) {
        if (c == '\n') {
            newlines++;
            if (newlines >= 2 && !out.empty()) break;
            if (!last_space && !out.empty()) out += ' ';
            last_space = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (!last_space && !out.empty()) out += ' ';
            last_space = true;
            continue;
        }
        if (newlines == 1 || !last_space || out.empty()) newlines = 0;
        newlines = 0;
        out += c;
        last_space = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Operators whose body (after the parts the walker handles) may start
// with declarations. Anywhere else a `declare` is misplaced.
const char *const kDeclaringMacros[] = {
    "WITH-OPEN-FILE", "WITH-OPEN-STREAM", "WITH-INPUT-FROM-STRING", "WITH-OUTPUT-TO-STRING",
    "WITH-HASH-TABLE-ITERATOR", "WITH-PACKAGE-ITERATOR", "PPRINT-LOGICAL-BLOCK", "DO-SYMBOLS",
    "DO-EXTERNAL-SYMBOLS", "DO-ALL-SYMBOLS", "WITH-COMPILATION-UNIT",
};

bool IsDeclaringMacro(const std::string &name) {
    for (const char *m : kDeclaringMacros) {
        if (name == m) return true;
    }
    return false;
}

// Calls that make the file's function names unknowable from its text.
const char *const kDynamicDefiners[] = {
    "EVAL", "LOAD", "COMPILE", "USE-PACKAGE", "IMPORT", "SHADOWING-IMPORT", "IN-READTABLE", "COMPILE-FILE",
    "LOAD-SYSTEM", "QUICKLOAD", "OOS", "OPERATE", "SET-MACRO-CHARACTER", "SET-DISPATCH-MACRO-CHARACTER",
};

bool IsDynamicDefiner(const std::string &name) {
    for (const char *m : kDynamicDefiners) {
        if (name == m) return true;
    }
    return false;
}

// --- The walker -------------------------------------------------------

class Walker {
  public:
    Walker(const std::vector<std::string> &lines, Index *index) : lines_(lines), ix_(index), r_(index->read) {
        Scope file;
        file.range.end_line = static_cast<int>(lines.size());
        ix_->scopes.push_back(file);
    }

    void Run() {
        for (int top : r_.top) CollectDefinitions(top, false);
        // `pkg::name` where pkg is the file's own package is the same
        // symbol as a bare `name`.
        if (own_packages_.empty()) {
            own_packages_.insert("CL-USER");
            own_packages_.insert("COMMON-LISP-USER");
        }
        CountDefinitions();
        for (int top : r_.top) Walk(top, 0);
        Resolve();
    }

  private:
    const std::vector<std::string> &lines_;
    Index *ix_;
    const LispReadResult &r_;
    int checks_off_ = 0;    // > 0: inside something the name checks must not judge
    int conditional_ = 0;   // > 0: inside #+/#- code for another platform
    int macro_body_ = 0;    // > 0: inside a defmacro/macrolet/define-compiler-macro definition

    const LispNode &At(int node) const { return r_.at(node); }
    bool Checked() const { return checks_off_ == 0; }

    /** @brief The canonical name of a symbol node in a CL-or-unqualified position, or "". */
    std::string ClName(int node) const {
        const LispNode &n = At(node);
        if (n.kind != LispNodeKind::Symbol || n.uninterned || !LispIsUnqualifiedOrCl(n)) return std::string();
        return n.name;
    }

    bool IsSym(int node) const { return At(node).kind == LispNodeKind::Symbol; }
    bool IsList(int node) const { return At(node).kind == LispNodeKind::List; }
    const std::vector<int> &Kids(int node) const { return At(node).kids; }
    int Kid(int node, size_t i) const {
        const std::vector<int> &k = Kids(node);
        return i < k.size() ? k[i] : -1;
    }

    void Diag(const LispRange &range, LispLspSeverity sev, const char *code, const std::string &message) {
        LispLspDiagnostic d;
        d.range = range;
        d.severity = sev;
        d.code = code;
        d.message = message;
        ix_->diags.push_back(d);
    }

    /** @brief Same, but only in code the checks run on. */
    void CheckedDiag(const LispRange &range, LispLspSeverity sev, const char *code, const std::string &message) {
        if (Checked()) Diag(range, sev, code, message);
    }

    std::string Display(int node) const {
        const LispNode &n = At(node);
        return n.kind == LispNodeKind::Symbol ? n.text : SourceText(lines_, n.range);
    }

    // --- Scopes and bindings -------------------------------------------

    int NewScope(int parent, int node) {
        Scope s;
        s.parent = parent;
        s.range = At(node).range;
        ix_->scopes.push_back(s);
        return static_cast<int>(ix_->scopes.size()) - 1;
    }

    int Bind(int name_node, Ns ns, BK kind, int scope, const std::string &origin) {
        const LispNode &n = At(name_node);
        Binding b;
        b.name = n.name;
        b.ns = ns;
        b.kind = kind;
        b.node = name_node;
        b.range = NameRange(n);
        b.def_range = n.range;
        b.scope = scope;
        b.origin = origin;
        b.conditional = conditional_ > 0;
        ix_->bindings.push_back(b);
        const int id = static_cast<int>(ix_->bindings.size()) - 1;
        ix_->scopes[static_cast<size_t>(scope)].bindings.push_back(id);
        Occ o;
        o.node = name_node;
        o.name = n.name;
        o.package = n.package;
        o.ns = ns;
        o.range = b.range;
        o.binding = id;
        o.def = true;
        o.checked = Checked();
        o.scope = scope;
        ix_->occs.push_back(o);
        return id;
    }

    /** @brief Binds a lexical variable, reporting the shapes the standard forbids. */
    int BindVar(int name_node, BK kind, int scope, const std::string &origin, bool unused_check) {
        const LispNode &n = At(name_node);
        if (LispIsKeyword(n) || n.name == "T" || n.name == "NIL" ||
            (LispIsUnqualifiedOrCl(n) && LispLspFindCl(n.name) != nullptr &&
             LispLspClHasKind(*LispLspFindCl(n.name), "constant")) ||
            IsFileConstant(n.name)) {
            CheckedDiag(n.range, LispLspSeverity::Error, "bind-constant",
                        "`" + n.text + "` is a constant and cannot be bound as a variable.");
            return -1;
        }
        const int id = Bind(name_node, Ns::Var, kind, scope, origin);
        Binding &b = ix_->bindings[static_cast<size_t>(id)];
        b.special = IsKnownSpecial(n);
        b.declared_special = IsDeclaredSpecial(n);
        b.check_unused = unused_check && Checked() && conditional_ == 0 && !b.special;
        if (IsEarmuffed(n.name) && !b.declared_special && kind == BK::Lexical && Checked() && conditional_ == 0 &&
            n.package.empty()) {
            pending_earmuffs_.push_back(id);
        }
        return id;
    }

    bool IsFileConstant(const std::string &name) const {
        for (const Binding &b : ix_->bindings) {
            if (b.scope == 0 && b.ns == Ns::Var && b.kind == BK::Constant && b.name == name) return true;
        }
        return false;
    }

    /** @brief A name the file or CL makes special: binding it is dynamic, never an unused lexical. */
    bool IsKnownSpecial(const LispNode &n) const { return IsEarmuffed(n.name) || IsDeclaredSpecial(n); }

    bool IsDeclaredSpecial(const LispNode &n) const {
        if (LispIsUnqualifiedOrCl(n)) {
            const LispClDecl *d = LispLspFindCl(n.name);
            if (d != nullptr && LispLspClHasKind(*d, "variable")) return true;
        }
        for (const Binding &b : ix_->bindings) {
            if (b.scope == 0 && b.ns == Ns::Var && b.kind == BK::Global && b.name == n.name) return true;
        }
        return false;
    }

    int Lookup(const std::string &name, Ns ns, int scope) const {
        for (int s = scope; s >= 0;) {
            const Scope &sc = ix_->scopes[static_cast<size_t>(s)];
            for (auto it = sc.bindings.rbegin(); it != sc.bindings.rend(); ++it) {
                const Binding &b = ix_->bindings[static_cast<size_t>(*it)];
                if (b.ns == ns && b.name == name) return *it;
            }
            s = sc.parent;
        }
        return -1;
    }

    int LookupHere(const std::string &name, Ns ns, int scope) const {
        const Scope &sc = ix_->scopes[static_cast<size_t>(scope)];
        for (auto it = sc.bindings.rbegin(); it != sc.bindings.rend(); ++it) {
            const Binding &b = ix_->bindings[static_cast<size_t>(*it)];
            if (b.ns == ns && b.name == name) return *it;
        }
        return -1;
    }

    /** @brief Records a use of a symbol in a namespace. */
    int Use(int node, Ns ns, int scope, bool write, bool call) {
        const LispNode &n = At(node);
        Occ o;
        o.node = node;
        o.name = n.name;
        o.package = n.package;
        o.ns = ns;
        o.range = NameRange(n);
        o.write = write;
        o.call = call;
        o.checked = Checked() && conditional_ == 0;
        o.scope = scope;
        // A symbol from another package is never one of this file's
        // lexical bindings, whatever its name.
        if (ns != Ns::Data && (LispIsUnqualifiedOrCl(n) || own_packages_.count(n.package) > 0) && !n.uninterned) {
            o.binding = Lookup(n.name, ns, scope);
        }
        if (o.binding >= 0) Count(o.binding, write, o.checked);
        ix_->occs.push_back(o);
        return static_cast<int>(ix_->occs.size()) - 1;
    }

    void Count(int binding, bool write, bool checked) {
        Binding &b = ix_->bindings[static_cast<size_t>(binding)];
        if (write) {
            b.writes++;
        } else {
            b.reads++;
            if (checked) b.checked_reads++;
        }
    }

    // Globals are bound in a pre-pass, so nothing resolves late except
    // what the pre-pass could not see; this is a second chance for those.
    void Resolve() {
        for (Occ &o : ix_->occs) {
            if (o.binding >= 0 || o.ns == Ns::Data) continue;
            if (!(o.package.empty() || o.package == "CL" || o.package == "COMMON-LISP" ||
                  own_packages_.count(o.package) > 0)) {
                continue;
            }
            o.binding = Lookup(o.name, o.ns, o.scope);
            if (o.binding >= 0) Count(o.binding, o.write, o.checked);
        }
    }

    // --- Definitions (pre-pass) ------------------------------------------

    int AddGlobal(int name_node, Ns ns, BK kind, int form, const std::string &origin, bool conditional) {
        const int saved = conditional_;
        if (conditional) conditional_ = 1;
        const int id = Bind(name_node, ns, kind, 0, origin);
        conditional_ = saved;
        // The pre-pass occurrence is the definition; the walk records
        // uses only, so drop the one Bind made and let WalkDefName add it.
        ix_->occs.pop_back();
        Binding &b = ix_->bindings[static_cast<size_t>(id)];
        b.def_range = At(form).range;
        b.pkg = current_pkg_;
        ix_->def_binding.emplace(form, id);
        return id;
    }

    /** @brief A docstring at body position `i` of a form (only when more forms follow, or the form allows it). */
    std::string DocAt(int form, size_t i, bool needs_following) const {
        const std::vector<int> &k = Kids(form);
        for (size_t j = i; j < k.size(); j++) {
            const LispNode &n = At(k[j]);
            if (n.kind == LispNodeKind::String) {
                if (needs_following && j + 1 >= k.size()) return std::string();
                return n.name;
            }
            if (n.kind == LispNodeKind::List && !n.kids.empty() && ClName(n.kids[0]) == "DECLARE") continue;
            break;
        }
        return std::string();
    }

    void SetShape(int id, int ll_node, bool check_keys) {
        Binding &b = ix_->bindings[static_cast<size_t>(id)];
        b.has_shape = At(ll_node).kind == LispNodeKind::List || LispIsSymbolNamed(At(ll_node), "NIL");
        b.shape = ShapeOfNode(r_, ll_node);
        b.lambda_text = At(ll_node).kind == LispNodeKind::List ? SourceText(lines_, At(ll_node).range) : "()";
        b.check_keys = check_keys;
    }

    void CollectDefinitions(int form, bool conditional) {
        const LispNode &n = At(form);
        if (n.kind == LispNodeKind::Feature) {
            const FeatureState st = FeatureStateOf(r_, n);
            if (st == FeatureState::Dead || n.kids.size() < 2) return;
            CollectDefinitions(n.kids[1], conditional || st == FeatureState::Other);
            return;
        }
        if (n.kind != LispNodeKind::List || n.kids.empty()) return;
        const std::string op = ClName(n.kids[0]);
        if (op.empty()) {
            // A definer this server does not know (`define-foo`, a
            // library's `defclass*`) defines names it cannot see.
            const LispNode &head = At(n.kids[0]);
            if (head.kind == LispNodeKind::Symbol && (StartsWith(head.name, "DEF") || head.name.find("DEFINE") !=
                                                                                         std::string::npos)) {
                if (LispLspFindCl(head.name) == nullptr) dynamic_definitions_ = true;
            }
            return;
        }
        const int name = Kid(form, 1);
        if (op == "IN-PACKAGE") {
            if (name >= 0) {
                current_pkg_ = At(name).name;
                own_packages_.insert(current_pkg_);
            }
            return;
        }
        if (op == "PROGN" || op == "LOCALLY") {
            for (size_t i = 1; i < n.kids.size(); i++) CollectDefinitions(n.kids[i], conditional);
            return;
        }
        if (op == "EVAL-WHEN" || op == "LET" || op == "LET*" || op == "FLET" || op == "LABELS" || op == "MACROLET" ||
            op == "SYMBOL-MACROLET") {
            for (size_t i = 2; i < n.kids.size(); i++) CollectDefinitions(n.kids[i], conditional);
            return;
        }
        if (op == "DEFUN" || op == "DEFMACRO" || op == "DEFGENERIC" || op == "DEFINE-COMPILER-MACRO") {
            if (op == "DEFINE-COMPILER-MACRO") return;  // not a definition of the function itself
            const bool is_macro = op == "DEFMACRO";
            if (IsSym(name)) {
                const int id = AddGlobal(name, Ns::Func, is_macro ? BK::Macro : (op == "DEFGENERIC" ? BK::Generic
                                                                                                    : BK::Function),
                                         form, Lower(op), conditional);
                SetShape(id, Kid(form, 2), op == "DEFUN");
                if (op == "DEFGENERIC") {
                    for (size_t i = 3; i < n.kids.size(); i++) {
                        const int opt = n.kids[i];
                        if (IsList(opt) && LispIsKeyword(At(Kid(opt, 0))) && At(Kid(opt, 0)).name == "DOCUMENTATION" &&
                            At(Kid(opt, 1)).kind == LispNodeKind::String) {
                            ix_->bindings[static_cast<size_t>(id)].doc = At(Kid(opt, 1)).name;
                        }
                    }
                } else {
                    ix_->bindings[static_cast<size_t>(id)].doc = DocAt(form, 3, true);
                }
                if (is_macro && ExpandsIntoDefinition(form)) dynamic_definitions_ = true;
            } else if (IsList(name) && Kids(name).size() == 2 && ClName(Kid(name, 0)) == "SETF" && IsSym(Kid(name, 1))) {
                const int id = AddGlobal(Kid(name, 1), Ns::Func, BK::SetfFn, form, Lower(op), conditional);
                ix_->bindings[static_cast<size_t>(id)].name = "(SETF " + At(Kid(name, 1)).name + ")";
                SetShape(id, Kid(form, 2), false);
            }
            return;
        }
        if (op == "DEFMETHOD") {
            int target = name;
            std::string bname;
            if (IsSym(name)) {
                bname = At(name).name;
            } else if (IsList(name) && Kids(name).size() == 2 && ClName(Kid(name, 0)) == "SETF" && IsSym(Kid(name, 1))) {
                target = Kid(name, 1);
                bname = "(SETF " + At(target).name + ")";
            } else {
                return;
            }
            size_t ll = 2;
            while (ll < n.kids.size() && !IsList(n.kids[ll]) && !LispIsSymbolNamed(At(n.kids[ll]), "NIL")) ll++;
            const int ll_node = Kid(form, ll);
            const int existing = FindGlobal(bname, Ns::Func);
            if (existing >= 0) {
                Binding &b = ix_->bindings[static_cast<size_t>(existing)];
                ix_->def_binding.emplace(form, existing);
                if (b.kind == BK::Generic && b.lambda_text.empty() && b.has_shape) {
                    const LispLambdaShape s = ShapeOfNode(r_, ll_node);
                    if (s.required != b.shape.required) b.has_shape = false;
                }
                return;
            }
            const int id = AddGlobal(target, Ns::Func, BK::Generic, form, "defmethod", conditional);
            ix_->bindings[static_cast<size_t>(id)].name = bname;
            // Only the methods are known: the required count is fixed by
            // congruence, but any method may accept more keywords.
            Binding &b = ix_->bindings[static_cast<size_t>(id)];
            b.has_shape = IsList(ll_node);
            b.shape = ShapeOfNode(r_, ll_node);
            if (b.shape.key || b.shape.optional > 0) {
                b.shape.allow_other_keys = true;
                b.shape.rest = b.shape.rest || b.shape.key;
            }
            b.check_keys = false;
            return;
        }
        if (op == "DEFVAR" || op == "DEFPARAMETER" || op == "DEFCONSTANT" || op == "DEFINE-SYMBOL-MACRO") {
            if (!IsSym(name)) return;
            const BK kind = op == "DEFCONSTANT" ? BK::Constant
                                                : (op == "DEFINE-SYMBOL-MACRO" ? BK::GlobalSymbolMacro : BK::Global);
            const int id = AddGlobal(name, Ns::Var, kind, form, Lower(op), conditional);
            if (op != "DEFINE-SYMBOL-MACRO") ix_->bindings[static_cast<size_t>(id)].doc = DocAt(form, 3, false);
            return;
        }
        if (op == "DECLAIM" || op == "PROCLAIM") {
            for (size_t i = 1; i < n.kids.size(); i++) {
                int spec = n.kids[i];
                if (At(spec).kind == LispNodeKind::Quote && !At(spec).kids.empty()) spec = At(spec).kids[0];
                if (IsList(spec) && ClName(Kid(spec, 0)) == "SPECIAL") {
                    for (size_t j = 1; j < Kids(spec).size(); j++) {
                        if (IsSym(Kids(spec)[j])) AddGlobal(Kids(spec)[j], Ns::Var, BK::Global, form, "declaim special",
                                                            conditional);
                    }
                }
            }
            return;
        }
        if (op == "DEFCLASS" || op == "DEFINE-CONDITION") {
            if (!IsSym(name)) return;
            AddGlobal(name, Ns::Type, BK::TypeName, form, Lower(op), conditional);
            const int slots = Kid(form, 3);
            if (!IsList(slots)) return;
            for (int slot : Kids(slots)) {
                if (!IsList(slot)) continue;
                const std::vector<int> &opts = Kids(slot);
                for (size_t i = 1; i + 1 < opts.size(); i += 2) {
                    const LispNode &key = At(opts[i]);
                    if (!LispIsKeyword(key)) continue;
                    const int val = opts[i + 1];
                    if ((key.name == "ACCESSOR" || key.name == "READER") && IsSym(val)) {
                        if (FindGlobal(At(val).name, Ns::Func) >= 0) continue;
                        const int id = AddGlobal(val, Ns::Func, BK::Accessor, form, "slot accessor", conditional);
                        Binding &b = ix_->bindings[static_cast<size_t>(id)];
                        b.has_shape = true;
                        b.shape.required = 1;
                        b.lambda_text = "(object)";
                    } else if (key.name == "WRITER" && IsSym(val)) {
                        if (FindGlobal(At(val).name, Ns::Func) >= 0) continue;
                        const int id = AddGlobal(val, Ns::Func, BK::Accessor, form, "slot writer", conditional);
                        Binding &b = ix_->bindings[static_cast<size_t>(id)];
                        b.has_shape = true;
                        b.shape.required = 2;
                        b.lambda_text = "(new-value object)";
                    }
                }
            }
            return;
        }
        if (op == "DEFSTRUCT") {
            CollectStruct(form, conditional);
            return;
        }
        if (op == "DEFTYPE") {
            if (IsSym(name)) AddGlobal(name, Ns::Type, BK::TypeName, form, "deftype", conditional);
            return;
        }
        if (op == "DEFINE-MODIFY-MACRO") {
            if (IsSym(name)) {
                const int id = AddGlobal(name, Ns::Func, BK::Macro, form, "define-modify-macro", conditional);
                // (name place . lambda-list)
                Binding &b = ix_->bindings[static_cast<size_t>(id)];
                b.shape = ShapeOfNode(r_, Kid(form, 2));
                b.shape.required++;
                b.has_shape = IsList(Kid(form, 2));
                b.lambda_text = "(place " + SourceText(lines_, At(Kid(form, 2)).range).substr(1);
            }
            return;
        }
        if (op == "DEFSETF" || op == "DEFINE-SETF-EXPANDER") {
            if (IsSym(name)) {
                const int id = AddGlobal(name, Ns::Func, BK::SetfFn, form, Lower(op), conditional);
                ix_->bindings[static_cast<size_t>(id)].name = "(SETF " + At(name).name + ")";
            }
            return;
        }
        // A definer this server does not know (`defsystem`, `define-foo`):
        // it defines names this file never spells out.
        if (LispLspFindCl(op) == nullptr && FindGlobal(op, Ns::Func) < 0 &&
            (StartsWith(op, "DEF") || op.find("DEFINE") != std::string::npos)) {
            dynamic_definitions_ = true;
        }
    }

    void CollectStruct(int form, bool conditional) {
        const int head = Kid(form, 1);
        int name_node = head;
        std::vector<int> options;
        if (IsList(head)) {
            name_node = Kid(head, 0);
            for (size_t i = 1; i < Kids(head).size(); i++) options.push_back(Kids(head)[i]);
        }
        if (!IsSym(name_node)) return;
        const std::string sname = At(name_node).name;
        AddGlobal(name_node, Ns::Type, BK::TypeName, form, "defstruct", conditional);
        std::string conc = sname + "-";
        std::vector<std::pair<std::string, int>> constructors;  // name, BOA lambda list node or -1
        bool default_constructor = true;
        std::string copier = "COPY-" + sname;
        std::string predicate = sname + "-P";
        bool included_unknown = false;
        std::vector<std::string> slot_names;
        for (int opt : options) {
            std::string oname;
            if (LispIsKeyword(At(opt))) oname = At(opt).name;
            else if (IsList(opt) && LispIsKeyword(At(Kid(opt, 0)))) oname = At(Kid(opt, 0)).name;
            const bool bare = LispIsKeyword(At(opt));
            const int arg = bare ? -1 : Kid(opt, 1);
            if (oname == "CONC-NAME") {
                if (bare || arg < 0 || LispIsSymbolNamed(At(arg), "NIL")) conc.clear();
                else if (IsSym(arg)) conc = At(arg).name;
                else if (At(arg).kind == LispNodeKind::String) conc = Upper(At(arg).name);
            } else if (oname == "CONSTRUCTOR") {
                if (!bare && arg >= 0 && LispIsSymbolNamed(At(arg), "NIL")) {
                    default_constructor = false;
                } else if (!bare && arg >= 0 && IsSym(arg)) {
                    default_constructor = false;
                    constructors.emplace_back(At(arg).name, Kid(opt, 2));
                }
            } else if (oname == "COPIER") {
                copier = (!bare && arg >= 0 && IsSym(arg) && !LispIsSymbolNamed(At(arg), "NIL")) ? At(arg).name : "";
            } else if (oname == "PREDICATE") {
                predicate =
                    (!bare && arg >= 0 && IsSym(arg) && !LispIsSymbolNamed(At(arg), "NIL")) ? At(arg).name : "";
            } else if (oname == "INCLUDE" && arg >= 0 && IsSym(arg)) {
                const auto it = struct_slots_.find(At(arg).name);
                if (it == struct_slots_.end()) {
                    included_unknown = true;
                } else {
                    slot_names = it->second;
                }
            }
        }
        const std::vector<int> &k = Kids(form);
        for (size_t i = 2; i < k.size(); i++) {
            const int slot = k[i];
            const int sn = IsList(slot) ? Kid(slot, 0) : slot;
            if (IsSym(sn)) slot_names.push_back(At(sn).name);
        }
        struct_slots_[sname] = slot_names;
        auto add_fn = [&](const std::string &fname, int required, const char *what) {
            if (fname.empty()) return;
            const int id = AddGlobal(name_node, Ns::Func, BK::StructFn, form, what, conditional);
            Binding &b = ix_->bindings[static_cast<size_t>(id)];
            b.name = fname;
            b.has_shape = required >= 0;
            b.shape.required = std::max(0, required);
            b.lambda_text = required == 1 ? "(object)" : "";
        };
        if (default_constructor) constructors.emplace_back("MAKE-" + sname, -1);
        for (const auto &c : constructors) {
            const int id = AddGlobal(name_node, Ns::Func, BK::StructFn, form, "defstruct constructor", conditional);
            Binding &b = ix_->bindings[static_cast<size_t>(id)];
            b.name = c.first;
            if (c.second >= 0) {
                b.has_shape = IsList(c.second);
                b.shape = ShapeOfNode(r_, c.second);
                b.lambda_text = SourceText(lines_, At(c.second).range);
            } else {
                b.has_shape = true;
                b.shape.key = true;
                b.shape.keys = slot_names;
                b.shape.allow_other_keys = included_unknown;
                std::string text = "(&key";
                for (const std::string &s : slot_names) text += " " + Lower(s);
                b.lambda_text = text + ")";
                b.check_keys = !included_unknown;
            }
        }
        add_fn(copier, 1, "defstruct copier");
        add_fn(predicate, 1, "defstruct predicate");
        for (const std::string &s : slot_names) add_fn(conc + s, 1, "defstruct accessor");
    }

    int FindGlobal(const std::string &name, Ns ns) const {
        for (int id : ix_->scopes[0].bindings) {
            const Binding &b = ix_->bindings[static_cast<size_t>(id)];
            if (b.ns == ns && b.name == name) return id;
        }
        return -1;
    }

    /** @brief A defmacro whose expansion contains a defining form: it defines names this file never spells. */
    bool ExpandsIntoDefinition(int form) const {
        bool found = false;
        std::vector<int> stack(Kids(form).begin(), Kids(form).end());
        while (!stack.empty() && !found) {
            const int node = stack.back();
            stack.pop_back();
            const LispNode &n = At(node);
            if (n.kind == LispNodeKind::List && !n.kids.empty() && IsSym(n.kids[0])) {
                const std::string &h = At(n.kids[0]).name;
                if ((StartsWith(h, "DEF") || h == "FLET" || h == "LABELS" || h == "MACROLET") && n.parent >= 0) {
                    // Only inside a backquote template, i.e. in the expansion.
                    for (int p = n.parent; p >= 0; p = At(p).parent) {
                        if (At(p).kind == LispNodeKind::Backquote) {
                            if (StartsWith(h, "DEF")) found = true;
                            break;
                        }
                    }
                }
            }
            for (int kid : n.kids) stack.push_back(kid);
        }
        return found;
    }

    void CountDefinitions() {
        std::map<std::pair<int, std::string>, std::vector<int>> by_name;
        for (int id : ix_->scopes[0].bindings) {
            const Binding &b = ix_->bindings[static_cast<size_t>(id)];
            by_name[{static_cast<int>(b.ns), b.name}].push_back(id);
        }
        // Methods add to a generic's count too: a name with a defun and a
        // defmethod, or two defuns under different #+, has no single
        // lambda list to check calls against.
        std::map<std::string, int> method_forms;
        for (const auto &kv : ix_->def_binding) {
            const LispNode &f = At(kv.first);
            if (!f.kids.empty() && ClName(f.kids[0]) == "DEFMETHOD") {
                method_forms[ix_->bindings[static_cast<size_t>(kv.second)].name]++;
            }
        }
        for (auto &kv : by_name) {
            const int count = static_cast<int>(kv.second.size());
            for (int id : kv.second) {
                Binding &b = ix_->bindings[static_cast<size_t>(id)];
                b.def_count = count;
                if (b.kind == BK::Function && method_forms.count(b.name) > 0) b.def_count += method_forms[b.name];
            }
            if (kv.first.first != static_cast<int>(Ns::Func)) continue;
            // Redefinition: the same function, macro or generic defined
            // twice outside any feature conditional.
            // Grouped by package as well: the same name defined after two
            // different in-package forms is two different symbols.
            std::map<std::string, std::vector<int>> real_by_pkg;
            for (int id : kv.second) {
                const Binding &b = ix_->bindings[static_cast<size_t>(id)];
                if (b.conditional) continue;
                if (b.kind == BK::Function || b.kind == BK::Macro ||
                    (b.kind == BK::Generic && b.origin == "defgeneric")) {
                    real_by_pkg[b.pkg].push_back(id);
                }
            }
            for (const auto &group : real_by_pkg) {
            const std::vector<int> &real = group.second;
            for (size_t i = 1; i < real.size(); i++) {
                const Binding &b = ix_->bindings[static_cast<size_t>(real[i])];
                const Binding &first = ix_->bindings[static_cast<size_t>(real[0])];
                Diag(b.range, LispLspSeverity::Warning, "redefinition",
                     "`" + Lower(b.name) + "` is already defined by the " + first.origin + " on line " +
                         std::to_string(first.range.line + 1) + "; this definition replaces it.");
            }
            }
        }
    }

  public:
    bool dynamic_definitions_ = false;
    std::vector<int> pending_earmuffs_;
    std::set<std::string> local_operator_names_;

  private:
    std::map<std::string, std::vector<std::string>> struct_slots_;
    std::string current_pkg_;
    std::set<std::string> own_packages_;

    // --- Walking --------------------------------------------------------

    struct ChecksOff {
        explicit ChecksOff(Walker *w) : w_(w) { w_->checks_off_++; }
        ~ChecksOff() { w_->checks_off_--; }
        ChecksOff(const ChecksOff &) = delete;
        ChecksOff &operator=(const ChecksOff &) = delete;
        Walker *w_;
    };

    /** @brief Records the defining occurrence of a name the pre-pass bound to `form`. */
    void AddDefOcc(int form, int name_node) {
        const auto it = ix_->def_binding.find(form);
        if (it == ix_->def_binding.end() || name_node < 0) return;
        const Binding &b = ix_->bindings[static_cast<size_t>(it->second)];
        const LispNode &n = At(name_node);
        Occ o;
        o.node = name_node;
        o.name = n.name;
        o.package = n.package;
        o.ns = b.ns;
        o.range = NameRange(n);
        o.binding = it->second;
        o.def = true;
        o.checked = Checked();
        ix_->occs.push_back(o);
    }

    /** @brief Records a symbol inside quoted data, for references and rename by name. */
    void UseData(int node) {
        const LispNode &n = At(node);
        if (n.kind != LispNodeKind::Symbol || LispIsKeyword(n)) return;
        Occ o;
        o.node = node;
        o.name = n.name;
        o.package = n.package;
        o.ns = Ns::Data;
        o.range = NameRange(n);
        // Quoted code inside a macro definition is expansion material
        // too, whether it is built with backquote or with `subst`.
        o.in_template = macro_body_ > 0;
        ix_->occs.push_back(o);
    }

    void WalkData(int node) {
        if (node < 0) return;
        std::vector<int> stack{node};
        while (!stack.empty()) {
            const int cur = stack.back();
            stack.pop_back();
            const LispNode &n = At(cur);
            if (n.kind == LispNodeKind::Symbol) UseData(cur);
            for (auto it = n.kids.rbegin(); it != n.kids.rend(); ++it) stack.push_back(*it);
        }
    }

    void WalkTemplate(int node, int scope, int depth) {
        const LispNode &n = At(node);
        switch (n.kind) {
            case LispNodeKind::Unquote:
            case LispNodeKind::Splice:
                if (n.kids.empty()) return;
                if (depth <= 1) {
                    Walk(n.kids[0], scope);
                } else {
                    WalkTemplate(n.kids[0], scope, depth - 1);
                }
                return;
            case LispNodeKind::Backquote:
                if (!n.kids.empty()) WalkTemplate(n.kids[0], scope, depth + 1);
                return;
            case LispNodeKind::Symbol:
                UseData(node);
                if (!ix_->occs.empty() && ix_->occs.back().node == node) ix_->occs.back().in_template = true;
                return;
            default: break;
        }
        for (int kid : n.kids) WalkTemplate(kid, scope, depth);
    }

    void Walk(int node, int scope) {
        if (node < 0) return;
        const LispNode &n = At(node);
        switch (n.kind) {
            case LispNodeKind::Symbol:
                if (LispIsKeyword(n) || n.uninterned) return;
                if (LispIsUnqualifiedOrCl(n) && (n.name == "T" || n.name == "NIL")) return;
                Use(node, Ns::Var, scope, false, false);
                return;
            case LispNodeKind::Quote: {
                if (n.kids.empty()) return;
                const LispNode &q = At(n.kids[0]);
                const bool self_eval = LispIsKeyword(q) || q.kind == LispNodeKind::Number ||
                                       q.kind == LispNodeKind::String || q.kind == LispNodeKind::Character;
                if (self_eval && conditional_ == 0) {
                    CheckedDiag(n.range, LispLspSeverity::Hint, "quoted-self-evaluating",
                                "`" + q.text + "` evaluates to itself; the quote does nothing.");
                }
                WalkData(n.kids[0]);
                return;
            }
            case LispNodeKind::Backquote:
                if (!n.kids.empty()) WalkTemplate(n.kids[0], scope, 1);
                return;
            case LispNodeKind::Unquote:
            case LispNodeKind::Splice:
                if (!n.kids.empty()) Walk(n.kids[0], scope);
                return;
            case LispNodeKind::Function:
                if (!n.kids.empty()) WalkFunctionRef(n.kids[0], scope);
                return;
            case LispNodeKind::ReadEval: {
                // Read-time code very often builds the code around it out
                // of quoted lists, so its quoted names count as mentions.
                ChecksOff off(this);
                macro_body_++;
                if (!n.kids.empty()) Walk(n.kids[0], scope);
                macro_body_--;
                // `#.(build-code ...)` can produce code that uses any
                // variable in scope, under a name computed at read time.
                if (!n.kids.empty() && IsList(n.kids[0])) ix_->read_evals.push_back(n.range);
                return;
            }
            case LispNodeKind::Feature: {
                if (n.kids.size() < 2) return;
                const FeatureState st = FeatureStateOf(r_, n);
                if (st == FeatureState::Dead) return;
                if (st == FeatureState::Active) {
                    Walk(n.kids[1], scope);
                    return;
                }
                ChecksOff off(this);
                conditional_++;
                Walk(n.kids[1], scope);
                conditional_--;
                return;
            }
            case LispNodeKind::Dispatch:
                if (!n.text.empty() && n.text.back() == ':') {
                    // `sb-kernel::(...)`: code read in another package.
                    ChecksOff off(this);
                    for (int kid : n.kids) Walk(kid, scope);
                    return;
                }
                WalkData(node);
                return;
            case LispNodeKind::Vector: WalkData(node); return;
            case LispNodeKind::List: WalkForm(node, scope); return;
            default: return;
        }
    }

    void WalkFunctionRef(int node, int scope) {
        const LispNode &n = At(node);
        if (n.kind == LispNodeKind::Symbol) {
            const int occ = Use(node, Ns::Func, scope, false, false);
            NoteUnknownOperator(occ);
            return;
        }
        if (n.kind == LispNodeKind::List && !n.kids.empty() && ClName(n.kids[0]) == "LAMBDA") {
            WalkLambdaLike(node, 1, 2, scope, "lambda", LLKind::Ordinary, -1);
            return;
        }
        WalkData(node);
    }

    /** @brief Remembers an operator nothing in the file or in CL defines, for undefined-function. */
    void NoteUnknownOperator(int occ_index) {
        const Occ &o = ix_->occs[static_cast<size_t>(occ_index)];
        if (o.binding >= 0 || !o.checked || !o.package.empty()) return;
        if (ClIsOperator(LispLspFindCl(o.name))) return;
        ix_->unknown_calls.push_back({o.name, o.range});
    }

    /**
     * @brief Counts every visible variable or local function named anywhere in a macro call's arguments as used.
     *
     * A macro this server cannot expand may put any of its argument
     * symbols in evaluated position -- `(do-blocks (block component) ...)`
     * evaluates `component` even though, read as code, `(block component)`
     * would make it a block name. Unused-variable reports stay quiet
     * about anything a macro call mentions.
     */
    void SweepMacroArgs(int form, size_t from, int scope) {
        std::vector<int> stack;
        const std::vector<int> &k = Kids(form);
        for (size_t i = from; i < k.size(); i++) stack.push_back(k[i]);
        while (!stack.empty()) {
            const int node = stack.back();
            stack.pop_back();
            const LispNode &n = At(node);
            if (n.kind == LispNodeKind::Symbol) {
                if (!n.package.empty() && !LispIsUnqualifiedOrCl(n)) continue;
                const int var = Lookup(n.name, Ns::Var, scope);
                if (var >= 0) Count(var, false, false);
                const int fn = Lookup(n.name, Ns::Func, scope);
                if (fn >= 0 && ix_->bindings[static_cast<size_t>(fn)].scope != 0) Count(fn, false, false);
                continue;
            }
            for (int kid : n.kids) stack.push_back(kid);
        }
    }

    /**
     * @brief Remembers a call to a `with-...`/`do-...` macro from elsewhere.
     *
     * Those are the macros that capture variables from their call site
     * by name (`(let ((testp test)) (with-member-test ...))` in SBCL's
     * own seq.lisp), so a binding whose scope holds one is not reported
     * unused: its use may be in the expansion.
     */
    void NoteCapturingMacro(int form, const std::string &op) {
        if (StartsWith(op, "WITH-") || StartsWith(op, "DO-")) ix_->capturing_calls.push_back(At(form).range);
    }

    /** @brief An element whose presence depends on the platform (#+/#-) or the readtable (an unknown `#x`). */
    bool HasUncertainElement(int form, size_t from) const {
        const std::vector<int> &k = Kids(form);
        for (size_t i = from; i < k.size(); i++) {
            const LispNodeKind kind = At(k[i]).kind;
            if (kind == LispNodeKind::Feature || kind == LispNodeKind::Error) return true;
        }
        return false;
    }

    void WalkArgs(int form, size_t from, int scope) {
        const std::vector<int> &k = Kids(form);
        for (size_t i = from; i < k.size(); i++) Walk(k[i], scope);
    }

    // --- Calls ------------------------------------------------------------

    static std::string Plural(int n, const char *word) {
        return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
    }

    /** @brief Counts a call's arguments against a lambda list's shape. */
    void CheckArgs(int form, const LispLambdaShape &sh, const std::string &who, bool check_keys) {
        if (!Checked() || conditional_ > 0) return;
        const std::vector<int> &k = Kids(form);
        // `(f a #+sbcl b)`: the count depends on the platform.
        if (At(form).dotted || HasUncertainElement(form, 1)) return;
        const int argc = static_cast<int>(k.size()) - 1;
        const LispRange head = At(k[0]).range;
        const int positional = sh.required + sh.optional;
        if (argc < sh.required) {
            Diag(head, LispLspSeverity::Warning, "wrong-arg-count",
                 "`" + who + "` needs " + (sh.optional > 0 || sh.rest || sh.key ? "at least " : "") +
                     Plural(sh.required, "argument") + ", but this call passes " + std::to_string(argc) + ".");
            return;
        }
        if (!sh.rest && !sh.key && argc > positional) {
            Diag(head, LispLspSeverity::Warning, "wrong-arg-count",
                 "`" + who + "` takes " + (sh.optional > 0 ? "at most " : "") + Plural(positional, "argument") +
                     ", but this call passes " + std::to_string(argc) + ".");
            return;
        }
        if (!sh.key || argc <= positional) return;
        const int keyed = argc - positional;
        if (keyed % 2 != 0) {
            Diag(head, LispLspSeverity::Warning, "odd-keyword-args",
                 "`" + who + "` takes keyword arguments in `:name value` pairs, but this call leaves one unpaired.");
            return;
        }
        if (!check_keys || sh.allow_other_keys) return;
        std::vector<std::pair<int, std::string>> unknown;
        for (size_t i = static_cast<size_t>(positional) + 1; i + 1 < k.size(); i += 2) {
            const LispNode &key = At(k[i]);
            if (!LispIsKeyword(key)) return;  // a computed keyword: nothing to check
            if (key.name == "ALLOW-OTHER-KEYS") return;
            if (std::find(sh.keys.begin(), sh.keys.end(), key.name) == sh.keys.end()) {
                unknown.emplace_back(k[i], key.text);
            }
        }
        for (const auto &u : unknown) {
            std::string accepted;
            for (const std::string &name : sh.keys) accepted += (accepted.empty() ? ":" : ", :") + Lower(name);
            Diag(At(u.first).range, LispLspSeverity::Warning, "unknown-keyword",
                 "`" + who + "` does not accept the keyword `" + u.second + "`" +
                     (accepted.empty() ? std::string(".") : " (it takes " + accepted + ")."));
        }
    }

    void CheckClCall(int form, const std::string &op) {
        const LispClDecl *d = LispLspFindCl(op);
        if (d == nullptr || !ClIsOperator(d)) return;
        // DEFMETHOD's SBCL lambda list is (name &rest args); IF has its own
        // check; SETQ and SETF are pairs.
        if (op == "IF" || op == "SETQ" || op == "SETF" || op == "PSETQ" || op == "PSETF") return;
        const LispLambdaShape *sh = ClShape(d);
        if (sh == nullptr) return;
        if (std::strlen(d->lambda_list) == 0 && !LispLspClHasKind(*d, "special-operator")) {
            // An empty lambda list in the table means "no arguments" only
            // for a function introspection could read.
            if (!LispLspClHasKind(*d, "function")) return;
        }
        CheckArgs(form, *sh, Lower(op), false);
    }

    // --- Forms --------------------------------------------------------------

    void WalkForm(int form, int scope) {
        const std::vector<int> &k = Kids(form);
        if (k.empty()) return;
        const int head = k[0];
        const LispNode &h = At(head);
        if (h.kind == LispNodeKind::List) {
            if (!h.kids.empty() && ClName(h.kids[0]) == "LAMBDA") {
                WalkLambdaLike(head, 1, 2, scope, "lambda", LLKind::Ordinary, -1);
                WalkArgs(form, 1, scope);
                return;
            }
            // `((a 1) (b 2))` is not a call: it is data some macro (or a
            // file read as data) will take apart. Uses count; nothing is judged.
            ChecksOff off(this);
            WalkArgs(form, 0, scope);
            return;
        }
        if (h.kind == LispNodeKind::Feature) {
            // `(#+cmucl lisp::with-array-data #+sbcl sb-kernel:with-array-data ...)`:
            // the operator itself depends on the platform.
            ChecksOff off(this);
            WalkArgs(form, 0, scope);
            return;
        }
        if (h.kind != LispNodeKind::Symbol || LispIsKeyword(h)) {
            WalkArgs(form, 1, scope);
            return;
        }
        if (!h.package.empty() && !LispIsUnqualifiedOrCl(h)) {
            // Another package's operator: often a macro, and it may bind
            // anything.
            Use(head, Ns::Func, scope, false, true);
            NoteCapturingMacro(form, h.name);
            SweepMacroArgs(form, 1, scope);
            ChecksOff off(this);
            WalkArgs(form, 1, scope);
            return;
        }
        const std::string &op = h.name;
        if (!Checked()) {
            // Inside a macro this server does not know, `(key stream)` may
            // well be data the macro destructures: a visible variable of
            // that name counts as used.
            const int var = Lookup(op, Ns::Var, scope);
            if (var >= 0) Count(var, false, false);
        }
        if (op == "DECLARE") {
            WalkSpecial(form, op, scope);
            return;
        }
        const int local = Lookup(op, Ns::Func, scope);
        if (local >= 0 && ix_->bindings[static_cast<size_t>(local)].scope != 0) {
            Use(head, Ns::Func, scope, false, true);
            const Binding &b = ix_->bindings[static_cast<size_t>(local)];
            if (b.kind == BK::LocalMacro) {
                SweepMacroArgs(form, 1, scope);
                ChecksOff off(this);
                WalkArgs(form, 1, scope);
                return;
            }
            if (b.has_shape) CheckArgs(form, b.shape, Lower(op), b.check_keys);
            WalkArgs(form, 1, scope);
            return;
        }
        const LispClDecl *cl = LispLspFindCl(op);
        if (cl != nullptr && ClIsOperator(cl) && !h.uninterned) {
            Use(head, Ns::Func, scope, false, true);
            CheckClCall(form, op);
            if (WalkSpecial(form, op, scope)) return;
            WalkArgs(form, 1, scope);
            return;
        }
        const int occ = Use(head, Ns::Func, scope, false, true);
        const int global = ix_->occs[static_cast<size_t>(occ)].binding;
        if (global >= 0) {
            const Binding &b = ix_->bindings[static_cast<size_t>(global)];
            if (b.has_shape && b.def_count == 1) CheckArgs(form, b.shape, Lower(op), b.check_keys);
            if (b.kind == BK::Macro) {
                SweepMacroArgs(form, 1, scope);
                ChecksOff off(this);
                WalkArgs(form, 1, scope);
                return;
            }
            WalkArgs(form, 1, scope);
            return;
        }
        NoteUnknownOperator(occ);
        if (Checked() && conditional_ == 0 && IsDynamicDefiner(op)) dynamic_definitions_ = true;
        NoteCapturingMacro(form, op);
        SweepMacroArgs(form, 1, scope);
        ChecksOff off(this);
        WalkArgs(form, 1, scope);
    }

    /** @brief Assigns to a symbol (setq/setf target). */
    void WriteVar(int node, int scope, bool also_reads) {
        const LispNode &n = At(node);
        if (n.kind != LispNodeKind::Symbol) {
            Walk(node, scope);
            return;
        }
        if (LispIsKeyword(n) || (LispIsUnqualifiedOrCl(n) && (n.name == "T" || n.name == "NIL"))) {
            CheckedDiag(n.range, LispLspSeverity::Error, "bind-constant", "`" + n.text + "` is a constant.");
            return;
        }
        const int occ = Use(node, Ns::Var, scope, true, false);
        const Occ &o = ix_->occs[static_cast<size_t>(occ)];
        if (also_reads && o.binding >= 0) Count(o.binding, false, o.checked);
        if (o.binding < 0 && o.checked && n.package.empty() && !IsEarmuffed(n.name) &&
            !ClIsVariable(LispLspFindCl(n.name))) {
            ix_->unknown_setqs.push_back({n.name, o.range});
        }
    }

    void WalkPlace(int place, int scope, bool also_reads) {
        const LispNode &p = At(place);
        if (p.kind == LispNodeKind::Symbol) {
            WriteVar(place, scope, also_reads);
            return;
        }
        if (p.kind != LispNodeKind::List || p.kids.empty() || !IsSym(p.kids[0])) {
            Walk(place, scope);
            return;
        }
        const std::string op = ClName(p.kids[0]);
        if (op == "THE" && p.kids.size() == 3) {
            WalkPlace(p.kids[2], scope, also_reads);
            return;
        }
        if (op == "VALUES") {
            for (size_t i = 1; i < p.kids.size(); i++) WalkPlace(p.kids[i], scope, also_reads);
            return;
        }
        if (op == "FDEFINITION" || op == "SYMBOL-FUNCTION" || op == "MACRO-FUNCTION" ||
            op == "COMPILER-MACRO-FUNCTION") {
            dynamic_definitions_ = true;  // (setf (fdefinition 'name) ...) defines a function by name
        }
        // (setf (accessor x) v): the accessor is used, as a place -- and
        // so is a local `(setf accessor)` function, if there is one.
        Use(p.kids[0], Ns::Func, scope, false, false);
        const int setf_fn = Lookup("(SETF " + At(p.kids[0]).name + ")", Ns::Func, scope);
        if (setf_fn >= 0) Count(setf_fn, false, Checked());
        WalkArgs(place, 1, scope);
    }

    bool WalkSpecial(int form, const std::string &op, int scope) {
        const std::vector<int> &k = Kids(form);
        const size_t n = k.size();
        if (op == "QUOTE") {
            if (n > 1) WalkData(k[1]);
            return true;
        }
        if (op == "FUNCTION") {
            if (n > 1) WalkFunctionRef(k[1], scope);
            return true;
        }
        if (op == "DECLARE") {
            CheckedDiag(At(form).range, LispLspSeverity::Error, "misplaced-declare",
                        "A `declare` is only allowed at the start of a body that accepts declarations (defun, let, "
                        "lambda, ...), before any other form.");
            return true;
        }
        if (op == "SETQ" || op == "PSETQ" || op == "SETF" || op == "PSETF") {
            if ((n - 1) % 2 != 0 && !HasUncertainElement(form, 1)) {
                CheckedDiag(At(k[0]).range, LispLspSeverity::Error, "odd-setf-args",
                            "`" + Lower(op) + "` takes place/value pairs, but the last place has no value.");
            }
            const bool places = op == "SETF" || op == "PSETF";
            for (size_t i = 1; i < n; i += 2) {
                if (places) {
                    WalkPlace(k[i], scope, false);
                } else {
                    WriteVar(k[i], scope, false);
                }
                if (i + 1 < n) Walk(k[i + 1], scope);
            }
            return true;
        }
        if (op == "INCF" || op == "DECF" || op == "POP" || op == "REMF") {
            if (n > 1) WalkPlace(k[1], scope, true);
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "PUSH" || op == "PUSHNEW") {
            if (n > 1) Walk(k[1], scope);
            if (n > 2) WalkPlace(k[2], scope, true);
            WalkArgs(form, 3, scope);
            return true;
        }
        if (op == "SHIFTF" || op == "ROTATEF") {
            for (size_t i = 1; i < n; i++) {
                if (op == "SHIFTF" && i + 1 == n) {
                    Walk(k[i], scope);
                } else {
                    WalkPlace(k[i], scope, true);
                }
            }
            return true;
        }
        if (op == "MULTIPLE-VALUE-SETQ") {
            if (n > 1 && IsList(k[1])) {
                for (int v : Kids(k[1])) WriteVar(v, scope, false);
            }
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "IF") {
            if ((n < 3 || n > 4) && !HasUncertainElement(form, 1)) {
                CheckedDiag(At(k[0]).range, LispLspSeverity::Error, "if-arity",
                            "`if` takes a test, a then-form and an optional else-form (" +
                                std::to_string(n - 1) + " given). Wrap several forms in `progn`, or use `when`, "
                                                        "`unless` or `cond`.");
            }
            WalkArgs(form, 1, scope);
            return true;
        }
        if (op == "EQ") {
            for (size_t i = 1; i < n; i++) {
                const LispNode &a = At(k[i]);
                if (a.kind == LispNodeKind::Number || a.kind == LispNodeKind::Character) {
                    // A small integer or a character happens to work in every
                    // implementation (they are immediate), so that is
                    // information; a float, ratio or bignum genuinely fails.
                    bool small = a.kind == LispNodeKind::Character;
                    if (a.kind == LispNodeKind::Number) {
                        const std::string &t = a.text;
                        const size_t digits = t.find_first_not_of("+-");
                        small = digits != std::string::npos && t.find_first_not_of("0123456789", digits) == std::string::npos &&
                                t.size() - digits <= 9;
                    }
                    CheckedDiag(a.range, small ? LispLspSeverity::Information : LispLspSeverity::Warning,
                                "eq-on-number",
                                "`eq` on a number or character is unreliable: the same value need not be the same "
                                "object. Use `eql` (or `=`, `char=`).");
                }
            }
            WalkArgs(form, 1, scope);
            return true;
        }
        if (op == "LET" || op == "LET*") {
            WalkLet(form, scope, op == "LET*");
            return true;
        }
        if (op == "FLET" || op == "LABELS" || op == "MACROLET") {
            WalkLocalFunctions(form, scope, op);
            return true;
        }
        if (op == "SYMBOL-MACROLET") {
            const int s = NewScope(scope, form);
            if (n > 1 && IsList(k[1])) {
                for (int b : Kids(k[1])) {
                    if (IsList(b) && IsSym(Kid(b, 0))) {
                        Walk(Kid(b, 1), scope);
                        Bind(Kid(b, 0), Ns::Var, BK::SymbolMacro, s, "symbol-macrolet");
                    }
                }
            }
            WalkBody(form, 2, s, false, false);
            return true;
        }
        if (op == "LAMBDA") {
            WalkLambdaLike(form, 1, 2, scope, "lambda", LLKind::Ordinary, -1);
            return true;
        }
        if (op == "DEFUN" || op == "DEFMACRO" || op == "DEFINE-COMPILER-MACRO" || op == "DEFINE-SETF-EXPANDER" ||
            op == "DEFTYPE") {
            int name = Kid(form, 1);
            if (IsList(name) && Kids(name).size() == 2) name = Kid(name, 1);
            AddDefOcc(form, name);
            const bool macro_ll = op != "DEFUN";
            if (macro_ll) macro_body_++;
            WalkLambdaLike(form, 2, 3, scope, Lower(op), macro_ll ? LLKind::Macro : LLKind::Ordinary,
                           IsSym(Kid(form, 1)) ? Kid(form, 1) : (IsSym(name) ? name : -1));
            if (macro_ll) macro_body_--;
            return true;
        }
        if (op == "DEFSETF") {
            AddDefOcc(form, Kid(form, 1));
            if (n > 3 && IsList(k[2]) && IsList(k[3])) {
                // Long form: (defsetf access lambda-list (store-var*) body)
                const int s = NewScope(scope, form);
                BindLambdaList(k[2], s, LLKind::Ordinary, "defsetf");
                for (int v : Kids(k[3])) {
                    if (IsSym(v)) BindVar(v, BK::Param, s, "defsetf store variable", true);
                }
                WalkBody(form, 4, s, true, false);
            } else {
                WalkData(form);
            }
            return true;
        }
        if (op == "DEFGENERIC") {
            AddDefOcc(form, IsList(Kid(form, 1)) ? Kid(Kid(form, 1), 1) : Kid(form, 1));
            {
                // The lambda list binds nothing, but its shape is checked.
                const int s = NewScope(scope, form);
                BindLambdaList(Kid(form, 2), s, LLKind::Generic, "defgeneric");
            }
            for (size_t i = 3; i < n; i++) {
                const int opt = k[i];
                if (IsList(opt) && LispIsKeyword(At(Kid(opt, 0))) && At(Kid(opt, 0)).name == "METHOD") {
                    WalkMethod(opt, 1, scope, Kid(form, 1));
                } else {
                    WalkData(opt);
                }
            }
            return true;
        }
        if (op == "DEFMETHOD") {
            int name = Kid(form, 1);
            if (IsList(name) && Kids(name).size() == 2) name = Kid(name, 1);
            AddDefOcc(form, name);
            WalkMethod(form, 2, scope, IsSym(name) ? name : -1);
            return true;
        }
        if (op == "DEFVAR" || op == "DEFPARAMETER" || op == "DEFCONSTANT") {
            const int name = Kid(form, 1);
            AddDefOcc(form, name);
            if (IsSym(name) && conditional_ == 0) {
                const LispNode &nm = At(name);
                if (op == "DEFCONSTANT") {
                    if (!IsPlusNamed(nm.name)) {
                        CheckedDiag(nm.range, LispLspSeverity::Hint, "defconstant-naming",
                                    "Constants are conventionally named with plus signs: `+" + Lower(nm.name) + "+`.");
                    }
                } else if (!IsEarmuffed(nm.name)) {
                    CheckedDiag(nm.range, LispLspSeverity::Hint, "defvar-naming",
                                "`" + Lower(op) + "` makes `" + nm.text +
                                    "` special everywhere, even in `let` and parameter lists; the *earmuffs* "
                                    "convention (`*" + Lower(nm.name) + "*`) is what keeps that visible.");
                }
            }
            if (n > 2) Walk(k[2], scope);
            return true;
        }
        if (op == "DEFINE-SYMBOL-MACRO") {
            AddDefOcc(form, Kid(form, 1));
            if (n > 2) Walk(k[2], scope);
            return true;
        }
        if (op == "DEFCLASS" || op == "DEFINE-CONDITION") {
            WalkClass(form, scope);
            return true;
        }
        if (op == "DEFSTRUCT") {
            WalkStruct(form, scope);
            return true;
        }
        if (op == "DEFINE-MODIFY-MACRO") {
            AddDefOcc(form, Kid(form, 1));
            if (n > 3 && IsSym(k[3])) Use(k[3], Ns::Func, scope, false, false);
            return true;
        }
        if (op == "DEFPACKAGE" || op == "IN-PACKAGE" || op == "DECLAIM" || op == "TRACE" || op == "UNTRACE" ||
            op == "DEFINE-METHOD-COMBINATION" || op == "FORMATTER" || op == "DEFINE-SETF-METHOD") {
            WalkData(form);
            return true;
        }
        if (op == "BLOCK") {
            const int s = NewScope(scope, form);
            if (IsSym(Kid(form, 1))) Bind(Kid(form, 1), Ns::Block, BK::BlockName, s, "block");
            WalkArgs(form, 2, s);
            return true;
        }
        if (op == "RETURN-FROM") {
            if (IsSym(Kid(form, 1))) Use(Kid(form, 1), Ns::Block, scope, false, false);
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "TAGBODY") {
            const int s = NewScope(scope, form);
            WalkTagbody(form, 1, s);
            return true;
        }
        if (op == "GO") {
            if (IsSym(Kid(form, 1))) Use(Kid(form, 1), Ns::Tag, scope, false, false);
            return true;
        }
        if (op == "PROG" || op == "PROG*") {
            WalkLet(form, scope, op == "PROG*", true);
            return true;
        }
        if (op == "DO" || op == "DO*") {
            WalkDo(form, scope, op == "DO*");
            return true;
        }
        if (op == "DOLIST" || op == "DOTIMES" || op == "DO-SYMBOLS" || op == "DO-EXTERNAL-SYMBOLS" ||
            op == "DO-ALL-SYMBOLS") {
            const int spec = Kid(form, 1);
            const int s = NewScope(scope, form);
            if (IsList(spec)) {
                const std::vector<int> &sk = Kids(spec);
                for (size_t i = 1; i < sk.size() && i < 2; i++) Walk(sk[i], scope);
                if (!sk.empty() && IsSym(sk[0])) BindVar(sk[0], BK::LoopVar, s, Lower(op), false);
                for (size_t i = 2; i < sk.size(); i++) Walk(sk[i], s);
            }
            WalkBody(form, 2, s, false, true);
            return true;
        }
        if (op == "COND") {
            for (size_t i = 1; i < n; i++) {
                if (WalkFeatureClause(k[i], scope)) continue;
                if (IsList(k[i])) {
                    WalkArgs(k[i], 0, scope);
                } else {
                    Walk(k[i], scope);
                }
            }
            return true;
        }
        if (op == "CASE" || op == "ECASE" || op == "CCASE" || op == "TYPECASE" || op == "ETYPECASE" ||
            op == "CTYPECASE") {
            if (n > 1) Walk(k[1], scope);
            const bool types = op.find("TYPECASE") != std::string::npos;
            for (size_t i = 2; i < n; i++) {
                const int clause = k[i];
                if (WalkFeatureClause(clause, scope)) continue;
                if (!IsList(clause) || Kids(clause).empty()) continue;
                const int key = Kid(clause, 0);
                if (!types) CheckCaseKey(key);
                WalkData(key);
                WalkArgs(clause, 1, scope);
            }
            return true;
        }
        if (op == "HANDLER-CASE") {
            if (n > 1) Walk(k[1], scope);
            for (size_t i = 2; i < n; i++) {
                const int clause = k[i];
                if (WalkFeatureClause(clause, scope)) continue;
                if (!IsList(clause) || Kids(clause).empty()) continue;
                const int type = Kid(clause, 0);
                if (LispIsKeyword(At(type)) && At(type).name == "NO-ERROR") {
                    WalkLambdaLike(clause, 1, 2, scope, "handler-case :no-error clause", LLKind::Ordinary, -1);
                    continue;
                }
                WalkData(type);
                const int s = NewScope(scope, clause);
                const int vars = Kid(clause, 1);
                if (IsList(vars)) {
                    for (int v : Kids(vars)) {
                        if (IsSym(v)) BindVar(v, BK::Lexical, s, "handler-case clause", true);
                    }
                }
                WalkBody(clause, 2, s, false, false);
            }
            return true;
        }
        if (op == "HANDLER-BIND" || op == "RESTART-BIND") {
            const int bindings = Kid(form, 1);
            if (IsList(bindings)) {
                for (int b : Kids(bindings)) {
                    if (!IsList(b)) continue;
                    WalkData(Kid(b, 0));
                    WalkArgs(b, 1, scope);
                }
            }
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "RESTART-CASE") {
            if (n > 1) Walk(k[1], scope);
            for (size_t i = 2; i < n; i++) {
                const int clause = k[i];
                if (WalkFeatureClause(clause, scope)) continue;
                if (!IsList(clause) || Kids(clause).size() < 2) continue;
                const int s = NewScope(scope, clause);
                BindLambdaList(Kid(clause, 1), s, LLKind::Ordinary, "restart parameter");
                size_t body = 2;
                const std::vector<int> &ck = Kids(clause);
                while (body + 1 < ck.size() && LispIsKeyword(At(ck[body]))) {
                    WalkFunctionish(ck[body + 1], scope);
                    body += 2;
                }
                WalkBody(clause, body, s, false, false);
            }
            return true;
        }
        if (op == "DESTRUCTURING-BIND") {
            if (n > 2) Walk(k[2], scope);
            const int s = NewScope(scope, form);
            BindLambdaList(Kid(form, 1), s, LLKind::Macro, "destructuring-bind");
            WalkBody(form, 3, s, false, false);
            return true;
        }
        if (op == "MULTIPLE-VALUE-BIND") {
            if (n > 2) Walk(k[2], scope);
            const int s = NewScope(scope, form);
            std::vector<std::string> seen;
            if (IsList(Kid(form, 1))) {
                for (int v : Kids(Kid(form, 1))) {
                    if (!IsSym(v)) continue;
                    CheckDuplicate(v, &seen, "duplicate-binding");
                    BindVar(v, BK::Lexical, s, "multiple-value-bind", true);
                }
            }
            WalkBody(form, 3, s, false, false);
            return true;
        }
        if (op == "WITH-SLOTS" || op == "WITH-ACCESSORS") {
            if (n > 2) Walk(k[2], scope);
            const int s = NewScope(scope, form);
            if (IsList(Kid(form, 1))) {
                for (int e : Kids(Kid(form, 1))) {
                    if (IsSym(e)) {
                        Bind(e, Ns::Var, BK::SymbolMacro, s, Lower(op));
                    } else if (IsList(e) && IsSym(Kid(e, 0))) {
                        Bind(Kid(e, 0), Ns::Var, BK::SymbolMacro, s, Lower(op));
                        if (op == "WITH-ACCESSORS" && IsSym(Kid(e, 1))) {
                            Use(Kid(e, 1), Ns::Func, scope, false, false);
                        } else {
                            WalkData(Kid(e, 1));
                        }
                    }
                }
            }
            WalkBody(form, 3, s, false, false);
            return true;
        }
        if (op == "WITH-OPEN-FILE" || op == "WITH-OPEN-STREAM" || op == "WITH-INPUT-FROM-STRING" ||
            op == "WITH-OUTPUT-TO-STRING" || op == "WITH-HASH-TABLE-ITERATOR" || op == "WITH-PACKAGE-ITERATOR" ||
            op == "PPRINT-LOGICAL-BLOCK") {
            const int spec = Kid(form, 1);
            const int s = NewScope(scope, form);
            if (IsList(spec) && !Kids(spec).empty()) {
                WalkArgs(spec, 1, scope);
                const int var = Kid(spec, 0);
                // pprint-logical-block's stream symbol names the stream it
                // writes to as well as the one it binds.
                if (op == "PPRINT-LOGICAL-BLOCK" && IsSym(var)) Walk(var, scope);
                if (IsSym(var) && !LispIsSymbolNamed(At(var), "NIL")) {
                    if (op == "WITH-HASH-TABLE-ITERATOR" || op == "WITH-PACKAGE-ITERATOR") {
                        Bind(var, Ns::Func, BK::LocalMacro, s, Lower(op));
                    } else {
                        BindVar(var, BK::Lexical, s, Lower(op), false);
                    }
                }
            }
            WalkBody(form, 2, s, false, false);
            return true;
        }
        if (op == "WITH-COMPILATION-UNIT" || op == "WITH-SIMPLE-RESTART" || op == "PRINT-UNREADABLE-OBJECT" ||
            op == "WITH-CONDITION-RESTARTS") {
            if (n > 1 && IsList(k[1])) WalkArgs(k[1], op == "WITH-SIMPLE-RESTART" ? 1 : 0, scope);
            WalkBody(form, 2, scope, false, false, IsDeclaringMacro(op));
            return true;
        }
        if (op == "ASSERT") {
            if (n > 1) Walk(k[1], scope);
            if (n > 2 && IsList(k[2])) {
                for (int p : Kids(k[2])) WalkPlace(p, scope, true);
            }
            WalkArgs(form, 3, scope);
            return true;
        }
        if (op == "CHECK-TYPE") {
            if (n > 1) Walk(k[1], scope);
            if (n > 2) WalkData(k[2]);
            WalkArgs(form, 3, scope);
            return true;
        }
        if (op == "THE") {
            if (n > 1) WalkData(k[1]);
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "LOCALLY") {
            const int s = NewScope(scope, form);
            WalkBody(form, 1, s, false, false);
            return true;
        }
        if (op == "EVAL-WHEN") {
            if (n > 1) WalkData(k[1]);
            WalkArgs(form, 2, scope);
            return true;
        }
        if (op == "LOOP") {
            WalkLoop(form, scope);
            return true;
        }
        if (op == "SETF-FUNCTION") return false;
        if (op == "SET") {
            // (set 'x v) assigns a special by name.
            WalkArgs(form, 1, scope);
            return true;
        }
        if (op == "EVAL" || op == "LOAD" || op == "COMPILE" || op == "USE-PACKAGE" || op == "IMPORT" ||
            op == "SHADOWING-IMPORT" || op == "SET-MACRO-CHARACTER" || op == "SET-DISPATCH-MACRO-CHARACTER") {
            dynamic_definitions_ = true;
            if (op == "SET-MACRO-CHARACTER" || op == "SET-DISPATCH-MACRO-CHARACTER") ix_->custom_readtable = true;
            return false;
        }
        return false;
    }

    /** @brief A `#+f (clause ...)` clause: its uses count, nothing in it is judged. Returns whether it was one. */
    bool WalkFeatureClause(int clause, int scope) {
        if (At(clause).kind != LispNodeKind::Feature) return false;
        const LispNode &f = At(clause);
        if (f.kids.size() < 2 || FeatureStateOf(r_, f) == FeatureState::Dead) return true;
        SweepMacroArgs(clause, 1, scope);
        ChecksOff off(this);
        conditional_++;
        if (IsList(f.kids[1])) WalkArgs(f.kids[1], 0, scope);
        conditional_--;
        return true;
    }

    void WalkFunctionish(int node, int scope) {
        const LispNode &n = At(node);
        if (n.kind == LispNodeKind::Symbol) {
            Use(node, Ns::Func, scope, false, false);
        } else {
            Walk(node, scope);
        }
    }

    void CheckCaseKey(int key) {
        const LispNode &k = At(key);
        bool quoted = k.kind == LispNodeKind::Quote;
        if (k.kind == LispNodeKind::List) {
            for (int e : k.kids) {
                if (At(e).kind == LispNodeKind::Quote) quoted = true;
            }
        }
        if (quoted) {
            CheckedDiag(k.range, LispLspSeverity::Warning, "quoted-case-key",
                        "`case` keys are not evaluated, so `'a` here is the list `(quote a)` and matches the symbols "
                        "`quote` and `a`. Drop the quote.");
        }
    }

    void CheckDuplicate(int name_node, std::vector<std::string> *seen, const char *code) {
        const LispNode &n = At(name_node);
        if (std::find(seen->begin(), seen->end(), n.name) != seen->end()) {
            CheckedDiag(n.range, LispLspSeverity::Error, code,
                        "`" + n.text + "` is bound more than once in the same " +
                            (std::strcmp(code, "duplicate-parameter") == 0 ? "lambda list." : "binding form."));
        }
        seen->push_back(n.name);
    }

    // --- Binding forms --------------------------------------------------

    void WalkLet(int form, int scope, bool sequential, bool prog = false) {
        const int bindings = Kid(form, 1);
        const int s = NewScope(scope, form);
        std::vector<std::pair<int, int>> vars;  // name node, init node
        if (IsList(bindings)) {
            for (int b : Kids(bindings)) {
                const LispNode &bn = At(b);
                if (bn.kind == LispNodeKind::Symbol) {
                    vars.emplace_back(b, -1);
                } else if (bn.kind == LispNodeKind::List && !bn.kids.empty() && IsSym(bn.kids[0]) &&
                           HasUncertainElement(b, 1)) {
                    // (x #+sbcl a #-sbcl b): one init form per platform.
                    vars.emplace_back(bn.kids[0], -1);
                    ChecksOff off(this);
                    WalkArgs(b, 1, sequential ? s : scope);
                } else if (bn.kind == LispNodeKind::List && !bn.kids.empty() && bn.kids.size() <= 2 &&
                           IsSym(bn.kids[0]) && !bn.dotted) {
                    vars.emplace_back(bn.kids[0], bn.kids.size() == 2 ? bn.kids[1] : -1);
                } else if (bn.kind == LispNodeKind::Feature) {
                    ChecksOff off(this);
                    Walk(b, scope);
                    continue;
                } else {
                    CheckedDiag(bn.range, LispLspSeverity::Error, "malformed-binding",
                                "A binding must be a symbol, `(symbol)` or `(symbol init-form)`.");
                    Walk(b, scope);
                    continue;
                }
                if (sequential) {
                    Walk(vars.back().second, s);
                    BindVar(vars.back().first, BK::Lexical, s, prog ? "prog*" : "let*", true);
                }
            }
        } else if (bindings >= 0 && At(bindings).kind == LispNodeKind::Feature) {
            ChecksOff off(this);
            Walk(bindings, scope);
        } else if (bindings >= 0 && !LispIsSymbolNamed(At(bindings), "NIL")) {
            CheckedDiag(At(bindings).range, LispLspSeverity::Error, "malformed-binding",
                        "The bindings of a `let` must be a list.");
        }
        // Only parallel binding forbids a repeat: `let*` nests each
        // binding inside the previous one, so a name may be rebound.
        std::vector<std::string> seen;
        if (!sequential) {
            for (const auto &v : vars) CheckDuplicate(v.first, &seen, "duplicate-binding");
        }
        if (!sequential) {
            for (const auto &v : vars) Walk(v.second, scope);
            for (const auto &v : vars) BindVar(v.first, BK::Lexical, s, prog ? "prog" : "let", true);
        }
        if (prog) {
            WalkBody(form, 2, s, false, true);
        } else {
            WalkBody(form, 2, s, false, false);
        }
    }

    void WalkLocalFunctions(int form, int scope, const std::string &op) {
        const int defs = Kid(form, 1);
        const int s = NewScope(scope, form);
        std::vector<int> def_list;
        if (IsList(defs)) {
            for (int d : Kids(defs)) {
                if (IsList(d) && !Kids(d).empty()) def_list.push_back(d);
            }
        }
        auto name_of = [this](int d) {
            int name = Kid(d, 0);
            if (IsList(name) && Kids(name).size() == 2) name = Kid(name, 1);
            return IsSym(name) ? name : -1;
        };
        auto bind_fn = [&](int d) {
            const int name = name_of(d);
            if (name < 0) return;
            const bool macro = op == "MACROLET";
            const int id = Bind(name, Ns::Func, macro ? BK::LocalMacro : BK::LocalFunction, s, Lower(op));
            Binding &b = ix_->bindings[static_cast<size_t>(id)];
            if (IsList(Kid(d, 0))) b.name = "(SETF " + b.name + ")";
            b.has_shape = IsList(Kid(d, 1)) || LispIsSymbolNamed(At(Kid(d, 1)), "NIL");
            b.shape = ShapeOfNode(r_, Kid(d, 1));
            b.lambda_text = IsList(Kid(d, 1)) ? SourceText(lines_, At(Kid(d, 1)).range) : "()";
            b.check_keys = !macro;
            b.def_range = At(d).range;
            b.check_unused = !macro && Checked() && conditional_ == 0;
            b.doc = DocAt(d, 2, true);
        };
        if (op == "LABELS") {
            for (int d : def_list) bind_fn(d);
        }
        const bool macros = op == "MACROLET";
        if (macros) macro_body_++;
        for (int d : def_list) {
            const int name = name_of(d);
            WalkLambdaLike(d, 1, 2, op == "LABELS" ? s : scope, Lower(op),
                           op == "MACROLET" ? LLKind::Macro : LLKind::Ordinary, name);
        }
        if (macros) macro_body_--;
        if (op != "LABELS") {
            for (int d : def_list) bind_fn(d);
        }
        WalkBody(form, 2, s, false, false);
    }

    void WalkDo(int form, int scope, bool sequential) {
        const int specs = Kid(form, 1);
        const int s = NewScope(scope, form);
        std::vector<int> steps;
        std::vector<std::pair<int, int>> vars;
        if (IsList(specs)) {
            for (int sp : Kids(specs)) {
                if (IsSym(sp)) {
                    vars.emplace_back(sp, -1);
                } else if (IsList(sp) && IsSym(Kid(sp, 0)) && Kids(sp).size() <= 3) {
                    vars.emplace_back(Kid(sp, 0), Kid(sp, 1));
                    if (Kids(sp).size() == 3) steps.push_back(Kid(sp, 2));
                } else {
                    CheckedDiag(At(sp).range, LispLspSeverity::Error, "malformed-binding",
                                "A `do` variable must be `var`, `(var init)` or `(var init step)`.");
                    continue;
                }
                if (sequential) {
                    Walk(vars.back().second, s);
                    BindVar(vars.back().first, BK::Lexical, s, "do*", true);
                }
            }
        }
        if (!sequential) {
            for (const auto &v : vars) Walk(v.second, scope);
            for (const auto &v : vars) BindVar(v.first, BK::Lexical, s, "do", true);
        }
        for (int st : steps) Walk(st, s);
        if (IsList(Kid(form, 2))) WalkArgs(Kid(form, 2), 0, s);
        WalkBody(form, 3, s, false, true);
    }

    enum class LLKind { Ordinary, Macro, Method, Generic };

    int WalkLambdaLike(int form, size_t ll_index, size_t body_index, int outer, const std::string &origin,
                       LLKind kind, int block_name) {
        const int s = NewScope(outer, form);
        if (block_name >= 0) {
            Bind(block_name, Ns::Block, BK::BlockName, s, origin);
            ix_->occs.pop_back();  // the name's occurrence belongs to the definition, not the block
        }
        const int ll = Kid(form, ll_index);
        if (ll >= 0) BindLambdaList(ll, s, kind, origin);
        WalkBody(form, body_index, s, true, false);
        return s;
    }

    /** @brief (defmethod name qualifier* specialized-lambda-list body): `from` indexes the first qualifier. */
    void WalkMethod(int form, size_t from, int scope, int name) {
        size_t ll = from;
        const std::vector<int> &k = Kids(form);
        while (ll < k.size() && !IsList(k[ll]) && !LispIsSymbolNamed(At(k[ll]), "NIL")) ll++;
        WalkLambdaLike(form, ll, ll + 1, scope, "defmethod", LLKind::Method, name);
    }

    void BindParam(int name_node, int s, LLKind kind, const std::string &origin, bool unused_check,
                   std::vector<std::string> *seen, bool aux = false) {
        if (!IsSym(name_node)) return;
        // &aux binds like let*, after the parameters: rebinding one of
        // them there is legal and a common idiom.
        if (!aux) CheckDuplicate(name_node, seen, "duplicate-parameter");
        if (kind == LLKind::Generic) return;
        BindVar(name_node, BK::Param, s, origin, unused_check && kind != LLKind::Method);
    }

    void BindLambdaList(int ll, int s, LLKind kind, const std::string &origin) {
        std::vector<std::string> seen;
        BindLambdaListInto(ll, s, kind, origin, &seen);
    }

    void BindLambdaListInto(int ll, int s, LLKind kind, const std::string &origin, std::vector<std::string> *seen) {
        const LispNode &n = At(ll);
        if (LispIsSymbolNamed(n, "NIL")) return;
        if (n.kind == LispNodeKind::Symbol && kind == LLKind::Macro) {
            BindParam(ll, s, kind, origin, true, seen);  // (destructuring-bind x ...) / dotted tail
            return;
        }
        if (n.kind == LispNodeKind::Feature) {
            // `#+poll (&optional x) #-poll (&aux x)`: the first variant is the
            // one bound; neither is judged.
            ChecksOff off(this);
            if (n.kids.size() >= 2) BindLambdaListInto(n.kids[1], s, kind, origin, seen);
            return;
        }
        if (n.kind != LispNodeKind::List) {
            CheckedDiag(n.range, LispLspSeverity::Error, "malformed-lambda-list", "A lambda list must be a list.");
            return;
        }
        // A lambda list holding a reader macro this server does not know
        // (`(&optional (x #L!2))`) is not one it can judge.
        bool uncertain = false;
        std::vector<int> pending(n.kids.begin(), n.kids.end());
        while (!pending.empty() && !uncertain) {
            const int e = pending.back();
            pending.pop_back();
            if (At(e).kind == LispNodeKind::Error) uncertain = true;
            for (int kid : Kids(e)) pending.push_back(kid);
        }
        if (uncertain) checks_off_++;
        struct Restore {
            int *counter;
            bool active;
            ~Restore() {
                if (active) (*counter)--;
            }
        } restore{&checks_off_, uncertain};
        enum State { Req = 0, Opt = 1, Rest = 2, RestDone = 3, Key = 4, Aok = 5, Aux = 6 };
        State state = Req;
        bool skip_next = false;  // after &whole / &environment
        int optional_kw = -1;
        const std::string param_origin = "parameter (" + origin + ")";
        for (size_t i = 0; i < n.kids.size(); i++) {
            const int e = n.kids[i];
            const LispNode &en = At(e);
            if (n.dotted && i + 1 == n.kids.size()) {
                BindParam(e, s, kind, param_origin, true, seen);
                break;
            }
            if (en.kind == LispNodeKind::Feature) {
                ChecksOff off(this);
                Walk(e, s);
                continue;
            }
            if (skip_next) {
                skip_next = false;
                if (IsSym(e)) {
                    CheckDuplicate(e, seen, "duplicate-parameter");
                    if (kind != LLKind::Generic) BindVar(e, BK::Param, s, param_origin, false);
                } else if (IsList(e) && kind == LLKind::Macro) {
                    BindLambdaListInto(e, s, kind, origin, seen);
                }
                continue;
            }
            if (en.kind == LispNodeKind::Symbol && !en.name.empty() && en.name[0] == '&') {
                State next = state;
                const std::string &kw = en.name;
                if (kw == "&OPTIONAL") {
                    next = Opt;
                    optional_kw = e;
                } else if (kw == "&REST" || kw == "&BODY") {
                    next = Rest;
                } else if (kw == "&KEY") {
                    next = Key;
                    if (optional_kw >= 0 && kind != LLKind::Generic && conditional_ == 0) {
                        CheckedDiag(en.range, LispLspSeverity::Information, "optional-and-key",
                                    "`&optional` together with `&key`: a call that leaves an optional out passes its "
                                    "first keyword as that optional instead.");
                    }
                } else if (kw == "&ALLOW-OTHER-KEYS") {
                    if (state != Key) {
                        CheckedDiag(en.range, LispLspSeverity::Error, "malformed-lambda-list",
                                    "`&allow-other-keys` must follow `&key`.");
                    }
                    next = Aok;
                } else if (kw == "&AUX") {
                    next = Aux;
                } else if (kw == "&WHOLE" || kw == "&ENVIRONMENT") {
                    skip_next = true;
                    continue;
                } else {
                    // An implementation's own keyword: stop judging.
                    return;
                }
                if (static_cast<int>(next) < static_cast<int>(state) ||
                    (next == state && next != Req && kw != "&ALLOW-OTHER-KEYS") || (state == Rest && next != Rest)) {
                    CheckedDiag(en.range, LispLspSeverity::Error, "malformed-lambda-list",
                                state == Rest ? "`&rest`/`&body` must be followed by exactly one variable."
                                              : "`" + Lower(kw) + "` is out of order: lambda-list keywords go "
                                                                  "&optional, &rest, &key, &allow-other-keys, &aux.");
                }
                state = next;
                continue;
            }
            switch (state) {
                case Req:
                    if (en.kind == LispNodeKind::Symbol) {
                        BindParam(e, s, kind, param_origin, true, seen);
                    } else if (en.kind == LispNodeKind::List && kind == LLKind::Method) {
                        // (param specializer): specializer is a class name or (eql form)
                        BindParam(Kid(e, 0), s, kind, param_origin, false, seen);
                        const int spec = Kid(e, 1);
                        if (IsList(spec) && ClName(Kid(spec, 0)) == "EQL") {
                            Walk(Kid(spec, 1), s);
                        } else {
                            WalkData(spec);
                        }
                    } else if (en.kind == LispNodeKind::List && kind == LLKind::Macro) {
                        BindLambdaListInto(e, s, kind, origin, seen);
                    } else if (kind != LLKind::Generic) {
                        CheckedDiag(en.range, LispLspSeverity::Error, "malformed-lambda-list",
                                    "A required parameter must be a symbol.");
                    }
                    break;
                case Opt:
                case Key:
                case Aux:
                    if (en.kind == LispNodeKind::Symbol) {
                        BindParam(e, s, kind, param_origin, true, seen, state == Aux);
                    } else if (en.kind == LispNodeKind::List && !en.kids.empty()) {
                        int var = en.kids[0];
                        if (state == Key && IsList(var)) var = Kid(var, 1);  // ((:kw var) ...)
                        if (en.kids.size() > 1 && kind != LLKind::Generic) Walk(en.kids[1], s);
                        if (IsSym(var)) {
                            BindParam(var, s, kind, param_origin, true, seen, state == Aux);
                        } else if (IsList(var) && kind == LLKind::Macro) {
                            BindLambdaListInto(var, s, kind, origin, seen);
                        }
                        if (state != Aux && en.kids.size() > 2 && IsSym(en.kids[2])) {
                            BindParam(en.kids[2], s, kind, param_origin, true, seen);
                        }
                    }
                    break;
                case Rest:
                    if (IsSym(e)) {
                        BindParam(e, s, kind, param_origin, true, seen);
                    } else if (IsList(e) && kind == LLKind::Macro) {
                        BindLambdaListInto(e, s, kind, origin, seen);
                    }
                    state = RestDone;
                    break;
                case RestDone:
                case Aok:
                    CheckedDiag(en.range, LispLspSeverity::Error, "malformed-lambda-list",
                                state == RestDone ? "Only one variable may follow `&rest`/`&body`."
                                                  : "Nothing but `&aux` may follow `&allow-other-keys`.");
                    break;
            }
        }
        if (state == Rest && !skip_next) {
            CheckedDiag(n.range, LispLspSeverity::Error, "malformed-lambda-list",
                        "`&rest`/`&body` must be followed by a variable.");
        }
    }

    /** @brief A body: optional docstring, leading declarations, then forms (a tagbody's when `tagbody`). */
    void WalkBody(int form, size_t from, int s, bool allow_doc, bool tagbody, bool allow_declare = true) {
        const std::vector<int> &k = Kids(form);
        size_t i = from;
        bool doc_seen = false;
        while (i < k.size()) {
            const LispNode &e = At(k[i]);
            if (allow_doc && !doc_seen && e.kind == LispNodeKind::String && i + 1 < k.size()) {
                doc_seen = true;
                i++;
                continue;
            }
            if (allow_declare && e.kind == LispNodeKind::List && !e.kids.empty() && ClName(e.kids[0]) == "DECLARE") {
                ProcessDeclare(k[i], s);
                i++;
                continue;
            }
            // `#.(optimize-settings)`: whatever the reader computes there is
            // usually a declaration, so the declaration phase goes on.
            if (allow_declare && e.kind == LispNodeKind::ReadEval) {
                Walk(k[i], s);
                i++;
                continue;
            }
            // `#-genera (declare (string s))`: a declaration on some platforms.
            if (allow_declare && e.kind == LispNodeKind::Feature && e.kids.size() >= 2 && IsList(e.kids[1]) &&
                !Kids(e.kids[1]).empty() && ClName(Kid(e.kids[1], 0)) == "DECLARE") {
                if (FeatureStateOf(r_, e) == FeatureState::Active) ProcessDeclare(e.kids[1], s);
                i++;
                continue;
            }
            break;
        }
        if (tagbody) {
            WalkTagbody(form, i, s);
            return;
        }
        for (; i < k.size(); i++) Walk(k[i], s);
    }

    void WalkTagbody(int form, size_t from, int s) {
        const std::vector<int> &k = Kids(form);
        for (size_t i = from; i < k.size(); i++) {
            if (IsSym(k[i])) Bind(k[i], Ns::Tag, BK::TagName, s, "tagbody tag");
        }
        for (size_t i = from; i < k.size(); i++) {
            const LispNode &e = At(k[i]);
            if (e.kind == LispNodeKind::Symbol || e.kind == LispNodeKind::Number) continue;
            Walk(k[i], s);
        }
    }

    void ProcessDeclare(int decl, int s) {
        for (size_t i = 1; i < Kids(decl).size(); i++) {
            const int spec = Kid(decl, i);
            if (!IsList(spec) || Kids(spec).empty()) continue;
            const std::string what = IsSym(Kid(spec, 0)) ? At(Kid(spec, 0)).name : std::string();
            const bool ignore = what == "IGNORE";
            const bool ignorable = what == "IGNORABLE";
            const bool special = what == "SPECIAL";
            for (size_t j = 1; j < Kids(spec).size(); j++) {
                int target = Kid(spec, j);
                Ns ns = Ns::Var;
                if (IsList(target) && ClName(Kid(target, 0)) == "FUNCTION") {
                    target = Kid(target, 1);
                    ns = Ns::Func;
                }
                if (!IsSym(target)) continue;
                // Every name in a declaration is an occurrence (so rename
                // follows it), but none of them is a use.
                const LispNode &tn = At(target);
                const int id = LookupHere(tn.name, ns, s);
                Occ o;
                o.node = target;
                o.name = tn.name;
                o.package = tn.package;
                o.ns = ns;
                o.range = NameRange(tn);
                o.binding = id >= 0 ? id : -1;
                o.scope = s;
                if (what == "TYPE" || ignore || ignorable || special || what == "DYNAMIC-EXTENT" ||
                    LispLspFindCl(what) != nullptr) {
                    ix_->occs.push_back(o);
                }
                if (id < 0) continue;
                Binding &b = ix_->bindings[static_cast<size_t>(id)];
                if (ignore) b.ignored = true;
                if (ignorable) b.ignorable = true;
                if (special) {
                    b.special = true;
                    b.declared_special = true;
                    b.check_unused = false;
                }
            }
        }
    }

    void WalkClass(int form, int scope) {
        const std::vector<int> &k = Kids(form);
        AddDefOcc(form, Kid(form, 1));
        if (k.size() > 2) WalkData(k[2]);
        const int slots = Kid(form, 3);
        if (IsList(slots)) {
            for (int slot : Kids(slots)) {
                if (!IsList(slot)) {
                    WalkData(slot);
                    continue;
                }
                WalkData(Kid(slot, 0));
                const std::vector<int> &opts = Kids(slot);
                for (size_t i = 1; i < opts.size(); i += 2) {
                    const LispNode &key = At(opts[i]);
                    const int val = i + 1 < opts.size() ? opts[i + 1] : -1;
                    if (LispIsKeyword(key) && key.name == "INITFORM") {
                        Walk(val, scope);
                    } else if (LispIsKeyword(key) &&
                               (key.name == "ACCESSOR" || key.name == "READER" || key.name == "WRITER") && IsSym(val)) {
                        const int id = FindGlobal(At(val).name, Ns::Func);
                        Occ o;
                        o.node = val;
                        o.name = At(val).name;
                        o.package = At(val).package;
                        o.ns = Ns::Func;
                        o.range = NameRange(At(val));
                        o.binding = id;
                        o.def = id >= 0 && ix_->bindings[static_cast<size_t>(id)].node == val;
                        ix_->occs.push_back(o);
                    } else {
                        WalkData(val);
                    }
                }
            }
        }
        for (size_t i = 4; i < k.size(); i++) {
            const int opt = k[i];
            if (!IsList(opt) || !LispIsKeyword(At(Kid(opt, 0)))) {
                WalkData(opt);
                continue;
            }
            const std::string &oname = At(Kid(opt, 0)).name;
            if (oname == "DEFAULT-INITARGS") {
                for (size_t j = 2; j < Kids(opt).size(); j += 2) Walk(Kid(opt, j), scope);
            } else if (oname == "REPORT") {
                WalkFunctionish(Kid(opt, 1), scope);
            } else {
                WalkData(opt);
            }
        }
    }

    void WalkStruct(int form, int scope) {
        const int head = Kid(form, 1);
        AddDefOcc(form, IsList(head) ? Kid(head, 0) : head);
        if (IsList(head)) {
            for (size_t i = 1; i < Kids(head).size(); i++) {
                const int opt = Kid(head, i);
                if (IsList(opt) && LispIsKeyword(At(Kid(opt, 0))) &&
                    (At(Kid(opt, 0)).name == "PRINT-FUNCTION" || At(Kid(opt, 0)).name == "PRINT-OBJECT")) {
                    WalkFunctionish(Kid(opt, 1), scope);
                } else if (IsList(opt) && LispIsKeyword(At(Kid(opt, 0))) && At(Kid(opt, 0)).name == "CONSTRUCTOR" &&
                           IsList(Kid(opt, 2))) {
                    // A BOA lambda list: its defaults are code.
                    const int s = NewScope(scope, opt);
                    ChecksOff off(this);
                    BindLambdaList(Kid(opt, 2), s, LLKind::Generic, "constructor");
                } else {
                    WalkData(opt);
                }
            }
        }
        for (size_t i = 2; i < Kids(form).size(); i++) {
            const int slot = Kid(form, i);
            if (IsList(slot)) {
                WalkData(Kid(slot, 0));
                Walk(Kid(slot, 1), scope);
                for (size_t j = 2; j < Kids(slot).size(); j++) WalkData(Kid(slot, j));
            } else if (At(slot).kind != LispNodeKind::String) {
                WalkData(slot);
            }
        }
    }

    // --- LOOP -----------------------------------------------------------

    static bool IsLoopWord(const std::string &name) {
        static const char *const kExtra[] = {
            "EACH", "THE", "HASH-KEY", "HASH-KEYS", "HASH-VALUE", "HASH-VALUES", "SYMBOL", "SYMBOLS",
            "PRESENT-SYMBOL", "PRESENT-SYMBOLS", "EXTERNAL-SYMBOL", "EXTERNAL-SYMBOLS", "OF", "IT", "AND",
            "APPENDING", "NCONCING", "COUNTING", "MAXIMIZING", "MINIMIZING", "=",
        };
        if (LispLspFindLoopKeyword(name) != nullptr) return true;
        for (const char *w : kExtra) {
            if (name == w) return true;
        }
        return false;
    }

    std::string LoopWord(int node) const {
        const LispNode &n = At(node);
        if (n.kind != LispNodeKind::Symbol) return std::string();
        return IsLoopWord(n.name) ? n.name : std::string();
    }

    void BindLoopVars(int node, int s, std::set<int> *skip) {
        const LispNode &n = At(node);
        skip->insert(node);
        if (n.kind == LispNodeKind::Symbol) {
            if (!LispIsSymbolNamed(n, "NIL")) BindVar(node, BK::LoopVar, s, "loop", false);
            return;
        }
        if (n.kind == LispNodeKind::List) {
            for (int kid : n.kids) BindLoopVars(kid, s, skip);
        }
    }

    void WalkLoop(int form, int scope) {
        const int s = NewScope(scope, form);
        const std::vector<int> &k = Kids(form);
        std::set<int> skip;
        // Pass 1: every variable the loop binds, wherever it appears --
        // `finally (return acc)` may come before `into acc` in the text.
        std::string last_binder;
        for (size_t i = 1; i < k.size(); i++) {
            const std::string w = LoopWord(k[i]);
            if (w.empty()) continue;
            if (w == "FOR" || w == "AS" || w == "WITH") last_binder = w;
            const bool binds_next = w == "FOR" || w == "AS" || w == "WITH" || w == "INTO" ||
                                    (w == "AND" && !last_binder.empty());
            if (w != "FOR" && w != "AS" && w != "WITH" && w != "AND" && !IsLoopForSubword(w)) last_binder.clear();
            if (binds_next && i + 1 < k.size()) {
                BindLoopVars(k[i + 1], s, &skip);
                i++;
                // An optional type spec after the variable: `for x fixnum = ...`
                if (i + 1 < k.size() && IsSym(k[i + 1]) && LoopWord(k[i + 1]).empty() && w != "INTO") {
                    skip.insert(k[i + 1]);
                }
            } else if (w == "USING" && i + 1 < k.size() && IsList(k[i + 1])) {
                const int u = k[i + 1];
                skip.insert(u);
                if (Kids(u).size() == 2 && IsSym(Kid(u, 1))) BindVar(Kid(u, 1), BK::LoopVar, s, "loop", false);
                i++;
            } else if (w == "NAMED" && i + 1 < k.size()) {
                if (IsSym(k[i + 1])) Bind(k[i + 1], Ns::Block, BK::BlockName, s, "loop named");
                skip.insert(k[i + 1]);
                i++;
            } else if (w == "OF-TYPE" && i + 1 < k.size()) {
                skip.insert(k[i + 1]);
                i++;
            }
        }
        // Pass 2: everything else that is not a loop keyword is a form --
        // including a variable that happens to be spelled like one, right
        // after a keyword that takes a form (`upto count`, `below end`).
        bool form_next = false;
        for (size_t i = 1; i < k.size(); i++) {
            if (skip.count(k[i]) > 0) {
                WalkData(k[i]);
                form_next = false;
                continue;
            }
            const std::string w = LoopWord(k[i]);
            if (!w.empty() && !(form_next && w != "IT")) {
                form_next = TakesForm(w);
                continue;
            }
            form_next = false;
            Walk(k[i], s);
        }
    }

    static bool TakesForm(const std::string &w) {
        static const char *const kFormWords[] = {
            "IN", "ON", "ACROSS", "FROM", "UPFROM", "DOWNFROM", "TO", "UPTO", "DOWNTO", "BELOW", "ABOVE", "BY",
            "THEN", "=", "REPEAT", "WHILE", "UNTIL", "ALWAYS", "NEVER", "THEREIS", "RETURN", "COLLECT",
            "COLLECTING", "APPEND", "APPENDING", "NCONC", "NCONCING", "COUNT", "COUNTING", "SUM", "SUMMING",
            "MAXIMIZE", "MAXIMIZING", "MINIMIZE", "MINIMIZING", "IF", "WHEN", "UNLESS", "OF",
        };
        for (const char *x : kFormWords) {
            if (w == x) return true;
        }
        return false;
    }

    static bool IsLoopForSubword(const std::string &w) {
        static const char *const kSub[] = {"IN", "ON", "ACROSS", "FROM", "UPFROM", "DOWNFROM", "TO", "UPTO",
                                           "DOWNTO", "BELOW", "ABOVE", "BY", "THEN", "=", "BEING", "THE", "EACH",
                                           "OF", "USING", "HASH-KEY", "HASH-KEYS", "HASH-VALUE", "HASH-VALUES",
                                           "SYMBOL", "SYMBOLS", "PRESENT-SYMBOL", "PRESENT-SYMBOLS",
                                           "EXTERNAL-SYMBOL", "EXTERNAL-SYMBOLS", "OF-TYPE"};
        for (const char *x : kSub) {
            if (w == x) return true;
        }
        return false;
    }
};

// --- Package context ----------------------------------------------------

/** @brief A package designator's canonical name: `:foo`, `#:foo`, `"FOO"`, `foo`. */
std::string DesignatorName(const LispNode &n) {
    if (n.kind == LispNodeKind::Symbol) return n.name;
    if (n.kind == LispNodeKind::String) return n.name;
    return std::string();
}

bool IsClPackageName(const std::string &name) { return name == "CL" || name == "COMMON-LISP"; }

/** @brief Decides whether the file's package is known to use only COMMON-LISP (see lisp_lsp.h). */
void AnalyzePackages(Index *ix) {
    const LispReadResult &r = ix->read;
    std::string current;
    bool conflicting = false;
    struct Pkg {
        bool cl_only = true;
    };
    std::map<std::string, Pkg> defined;
    for (int top : r.top) {
        const LispNode &f = r.at(top);
        if (f.kind != LispNodeKind::List || f.kids.empty()) continue;
        const LispNode &head = r.at(f.kids[0]);
        if (head.kind != LispNodeKind::Symbol) continue;
        const std::string op = head.name;
        if (op == "IN-PACKAGE" && f.kids.size() > 1) {
            const std::string name = DesignatorName(r.at(f.kids[1]));
            if (current.empty()) {
                current = name;
            } else if (current != name) {
                conflicting = true;
            }
        } else if (op.find("READTABLE") != std::string::npos || op.find("SYNTAX") != std::string::npos) {
            // `(in-readtable ...)`, `(in-ironclad-readtable)`,
            // `(enable-interpol-syntax)`: reader macros of the file's own.
            ix->custom_readtable = true;
        } else if ((op == "DEFPACKAGE" || op == "DEFINE-PACKAGE") && f.kids.size() > 1) {
            Pkg pkg;
            if (op == "DEFINE-PACKAGE") pkg.cl_only = head.package.empty() || IsClPackageName(head.package);
            for (size_t i = 2; i < f.kids.size(); i++) {
                const LispNode &opt = r.at(f.kids[i]);
                if (opt.kind != LispNodeKind::List || opt.kids.empty()) continue;
                const std::string oname = r.at(opt.kids[0]).name;
                if (oname == "USE") {
                    for (size_t j = 1; j < opt.kids.size(); j++) {
                        if (!IsClPackageName(DesignatorName(r.at(opt.kids[j])))) pkg.cl_only = false;
                    }
                } else if (oname == "MIX" || oname == "REEXPORT" || oname == "USE-REEXPORT" || oname == "MIX-REEXPORT") {
                    pkg.cl_only = false;
                } else if (oname == "IMPORT-FROM" || oname == "SHADOWING-IMPORT-FROM") {
                    for (size_t j = 2; j < opt.kids.size(); j++) ix->extra_known.insert(DesignatorName(r.at(opt.kids[j])));
                } else if (oname == "SHADOW") {
                    for (size_t j = 1; j < opt.kids.size(); j++) ix->extra_known.insert(DesignatorName(r.at(opt.kids[j])));
                }
            }
            defined[DesignatorName(r.at(f.kids[1]))] = pkg;
        }
    }
    if (conflicting) {
        ix->names_known = false;
        return;
    }
    if (current.empty() || current == "CL-USER" || current == "COMMON-LISP-USER") {
        for (const char *name : kSbclUserNames) ix->extra_known.insert(name);
        ix->names_known = true;
        ix->cl_user = true;
        return;
    }
    const auto it = defined.find(current);
    ix->names_known = it != defined.end() && it->second.cl_only;
}

/** @brief Reads and walks a document. */
Index BuildIndex(const std::vector<std::string> &lines) {
    Index ix;
    ix.read = LispRead(lines);
    AnalyzePackages(&ix);
    Walker w(lines, &ix);
    w.Run();
    if (w.dynamic_definitions_) ix.names_known = false;
    // In CL-USER an earmuffed name may be any implementation package's
    // special, inherited unqualified; only a file's own package is known.
    if (ix.names_known && !ix.cl_user) {
        for (int id : w.pending_earmuffs_) {
            const Binding &b = ix.bindings[static_cast<size_t>(id)];
            if (b.declared_special) continue;
            LispLspDiagnostic d;
            d.range = b.range;
            d.severity = LispLspSeverity::Information;
            d.code = "lexical-earmuffs";
            d.message = "`" + Lower(b.name) + "` has *earmuffs* but nothing declares it special, so this binding is "
                        "lexical: functions called from here will not see it.";
            ix.diags.push_back(d);
        }
    }
    return ix;
}

LispLspSeverity SeverityForReadError(const std::string &code) {
    if (code == "unknown-character-name" || code == "too-deep") return LispLspSeverity::Warning;
    return LispLspSeverity::Error;
}

bool DiagLess(const LispLspDiagnostic &a, const LispLspDiagnostic &b) {
    if (a.range.line != b.range.line) return a.range.line < b.range.line;
    return a.range.col < b.range.col;
}

const char *SeverityWord(LispLspSeverity s) {
    switch (s) {
        case LispLspSeverity::Error: return "error";
        case LispLspSeverity::Warning: return "warning";
        case LispLspSeverity::Information: return "info";
        case LispLspSeverity::Hint: return "hint";
    }
    return "error";
}

}  // namespace

namespace {

/** @brief The CL operator or file function closest to an unknown name (distance 1, or 2 for long names), or "". */
std::string ClosestOperator(const Index &ix, const std::string &name) {
    if (name.size() < 4) return std::string();
    const int limit = name.size() >= 8 ? 2 : 1;
    std::string best;
    int best_d = limit + 1;
    auto consider = [&](const std::string &cand) {
        const int d = EditDistance(name, cand, limit);
        if (d < best_d && d > 0) {
            best_d = d;
            best = cand;
        }
    };
    for (size_t i = 0; i < kLispClDeclCount; i++) {
        if (ClIsOperator(&kLispClDecls[i])) consider(kLispClDecls[i].name);
    }
    for (int id : ix.scopes[0].bindings) {
        const Binding &b = ix.bindings[static_cast<size_t>(id)];
        if (b.ns == Ns::Func) consider(b.name);
    }
    return best_d <= limit ? best : std::string();
}

}  // namespace

std::string LispLspFormatDiagnostic(const std::string &path, const LispLspDiagnostic &d) {
    return (path == "-" ? std::string("<stdin>") : path) + ":" + std::to_string(d.range.line + 1) + ":" +
           std::to_string(d.range.col + 1) + ": " + SeverityWord(d.severity) + ": " + d.message + " [" + d.code + "]";
}

bool LispLspDiagnosticFails(const LispLspDiagnostic &d) {
    return d.severity == LispLspSeverity::Error || d.severity == LispLspSeverity::Warning;
}

std::vector<LispLspDiagnostic> LispLspDiagnostics(const std::vector<std::string> &lines) {
    const Index ix = BuildIndex(lines);
    std::vector<LispLspDiagnostic> out;
    for (const LispReadError &e : ix.read.errors) {
        // A file that installs its own reader macros reads `#?` or `#[`
        // just fine; only the structural errors are still certain.
        if (ix.custom_readtable && (e.code == "unknown-dispatch" || e.code == "unreadable-object")) continue;
        LispLspDiagnostic d;
        d.range = e.range;
        d.severity = SeverityForReadError(e.code);
        d.code = e.code;
        d.message = e.message;
        out.push_back(d);
    }
    // The naming hints are a convention, and a file that consistently
    // follows another one (SBCL's own sources name constants without
    // plus signs) has made a choice rather than a slip: past three
    // definitions, a majority of unconventional names silences the hint.
    int constants = 0;
    int globals = 0;
    for (const Binding &b : ix.bindings) {
        if (b.scope != 0 || b.ns != Ns::Var) continue;
        if (b.kind == BK::Constant) constants++;
        if (b.kind == BK::Global && b.origin != "declaim special") globals++;
    }
    int constant_hints = 0;
    int global_hints = 0;
    for (const LispLspDiagnostic &d : ix.diags) {
        if (d.code == "defconstant-naming") constant_hints++;
        if (d.code == "defvar-naming") global_hints++;
    }
    const bool quiet_constants = constants >= 3 && constant_hints * 2 > constants;
    const bool quiet_globals = globals >= 3 && global_hints * 2 > globals;
    for (const LispLspDiagnostic &d : ix.diags) {
        if (quiet_constants && d.code == "defconstant-naming") continue;
        if (quiet_globals && d.code == "defvar-naming") continue;
        out.push_back(d);
    }

    // --- Unused and ignored variables ----------------------------------
    // A name mentioned inside a backquote template in the binding's own
    // scope is very likely used by the code a local macro expands into
    // (`(macrolet ((m () `(next-char))) ...)`): that counts as a use.
    // A name in any backquote template of the file may be captured on
    // purpose by a macro expanded inside the binding's scope.
    std::map<std::string, std::vector<LispRange>> data_mentions;
    std::set<std::string> template_mentions;
    for (const Occ &o : ix.occs) {
        if (o.ns != Ns::Data) continue;
        data_mentions[o.name].push_back(o.range);
        if (o.in_template) template_mentions.insert(o.name);
    }
    auto mentioned_in_scope = [&](const Binding &b) {
        if (template_mentions.count(b.name) > 0) return true;
        const LispRange &scope_range = ix.scopes[static_cast<size_t>(b.scope)].range;
        for (const LispRange &r : ix.capturing_calls) {
            if (!PosLess(r.line, r.col, scope_range.line, scope_range.col) &&
                PosLess(r.line, r.col, scope_range.end_line, scope_range.end_col)) {
                return true;
            }
        }
        for (const LispRange &r : ix.read_evals) {
            if (!PosLess(r.line, r.col, scope_range.line, scope_range.col) &&
                PosLess(r.line, r.col, scope_range.end_line, scope_range.end_col)) {
                return true;
            }
        }
        const auto it = data_mentions.find(b.name);
        if (it == data_mentions.end()) return false;
        const LispRange &sr = ix.scopes[static_cast<size_t>(b.scope)].range;
        for (const LispRange &r : it->second) {
            if (!PosLess(r.line, r.col, sr.line, sr.col) && PosLess(r.line, r.col, sr.end_line, sr.end_col)) return true;
        }
        return false;
    };
    for (const Binding &b : ix.bindings) {
        if (b.ns == Ns::Var && b.check_unused && !b.ignored && !b.ignorable && b.reads == 0 && !b.special) {
            if (b.name.empty() || b.name[0] == '_') continue;
            if (mentioned_in_scope(b)) continue;
            LispLspDiagnostic d;
            d.range = b.range;
            d.severity = LispLspSeverity::Warning;
            d.code = "unused-variable";
            d.message = "`" + Lower(b.name) + "` is " + (b.writes > 0 ? "assigned but never read" : "never used") +
                        "; declare it `(declare (ignore " + Lower(b.name) + "))` if that is deliberate.";
            out.push_back(d);
        }
        if (b.ns == Ns::Var && b.ignored && b.checked_reads > 0) {
            LispLspDiagnostic d;
            d.range = b.range;
            d.severity = LispLspSeverity::Warning;
            d.code = "used-ignored-variable";
            d.message = "`" + Lower(b.name) + "` is declared `ignore` but is used; declare it `ignorable` instead, "
                        "or drop the use.";
            out.push_back(d);
        }
        if (b.ns == Ns::Func && b.kind == BK::LocalFunction && b.check_unused && b.reads == 0 && !b.ignored &&
            !b.ignorable && !mentioned_in_scope(b)) {
            LispLspDiagnostic d;
            d.range = b.range;
            d.severity = LispLspSeverity::Warning;
            d.code = "unused-function";
            d.message = "The local function `" + Lower(b.name) + "` is never called.";
            out.push_back(d);
        }
    }

    // --- Undefined operators and variables -------------------------------
    if (ix.names_known) {
        std::set<std::string> distinct;
        for (const UnknownCall &u : ix.unknown_calls) {
            if (ix.extra_known.count(u.name) == 0) distinct.insert(u.name);
        }
        std::set<std::string> distinct_vars;
        for (const UnknownCall &u : ix.unknown_setqs) {
            if (ix.extra_known.count(u.name) == 0) distinct_vars.insert(u.name);
        }
        // More than a handful of unplaceable names means the file is part
        // of something bigger than itself; past that, reports are guesses.
        if (distinct.size() + distinct_vars.size() <= 5) {
            for (const UnknownCall &u : ix.unknown_calls) {
                if (distinct.count(u.name) == 0) continue;
                const std::string near = ClosestOperator(ix, u.name);
                // In CL-USER an unknown name is as likely to be the
                // implementation's own extension as a mistake; only a
                // near-miss of a known name is a confident report there.
                if (ix.cl_user && near.empty()) continue;
                LispLspDiagnostic d;
                d.range = u.range;
                d.severity = LispLspSeverity::Warning;
                d.code = "undefined-function";
                d.message = "`" + Lower(u.name) + "` is not defined in this file and is not a COMMON-LISP operator.";
                if (!near.empty()) d.message += " Did you mean `" + Lower(near) + "`?";
                out.push_back(d);
            }
            for (const UnknownCall &u : ix.unknown_setqs) {
                if (distinct_vars.count(u.name) == 0) continue;
                LispLspDiagnostic d;
                d.range = u.range;
                d.severity = LispLspSeverity::Warning;
                d.code = "setq-undefined";
                d.message = "`" + Lower(u.name) + "` is assigned here, but nothing binds it or declares it with "
                            "`defvar`/`defparameter`; the result is undefined.";
                out.push_back(d);
            }
        }
    }

    std::stable_sort(out.begin(), out.end(), DiagLess);
    return out;
}

// --- Position helpers --------------------------------------------------

namespace {

/** @brief The symbol node at a cursor position (inside it, or just past its end), or -1. */
int SymbolAt(const LispReadResult &r, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < r.nodes.size(); i++) {
        const LispNode &n = r.nodes[i];
        if (n.kind != LispNodeKind::Symbol) continue;
        if (LispRangeContains(n.range, line, col)) return static_cast<int>(i);
        if (n.range.end_line == line && n.range.end_col == col) best = static_cast<int>(i);
    }
    return best;
}

int OccAt(const Index &ix, int node) {
    if (node < 0) return -1;
    for (size_t i = 0; i < ix.occs.size(); i++) {
        if (ix.occs[i].node == node) return static_cast<int>(i);
    }
    return -1;
}

/** @brief A global (file-level) binding of a name, preferring the function namespace. */
int GlobalByName(const Index &ix, const std::string &name) {
    int found = -1;
    for (int id : ix.scopes[0].bindings) {
        const Binding &b = ix.bindings[static_cast<size_t>(id)];
        if (b.name != name) continue;
        if (b.ns == Ns::Func) return id;
        if (found < 0) found = id;
    }
    return found;
}

std::string BindingTitle(const Binding &b) {
    switch (b.kind) {
        case BK::Global: return "special variable";
        case BK::Constant: return "constant";
        case BK::GlobalSymbolMacro: return "global symbol macro";
        case BK::Function: return "function";
        case BK::Macro: return "macro";
        case BK::Generic: return "generic function";
        case BK::Accessor: return b.origin;
        case BK::StructFn: return b.origin;
        case BK::SetfFn: return "setf function";
        case BK::TypeName: return "type (" + b.origin + ")";
        case BK::Lexical: return b.special ? "special binding (" + b.origin + ")" : "lexical variable (" + b.origin + ")";
        case BK::Param: return b.origin;
        case BK::SymbolMacro: return "symbol macro (" + b.origin + ")";
        case BK::LocalFunction: return "local function (" + b.origin + ")";
        case BK::LocalMacro: return "local macro (" + b.origin + ")";
        case BK::BlockName: return "block name";
        case BK::TagName: return "go tag";
        case BK::LoopVar: return "loop variable (" + b.origin + ")";
    }
    return std::string();
}

std::string DescribeBinding(const Binding &b) {
    std::string text;
    const std::string name = Lower(b.name);
    if (b.ns == Ns::Func && !b.lambda_text.empty()) {
        std::string inner = b.lambda_text;
        if (inner.size() >= 2 && inner.front() == '(' && inner.back() == ')') inner = inner.substr(1, inner.size() - 2);
        text = "(" + name + (inner.empty() ? "" : " " + inner) + ")\n";
    } else {
        text = name + "\n";
    }
    text += BindingTitle(b) + ", " + (b.scope == 0 ? "defined" : "bound") + " on line " +
            std::to_string(b.range.line + 1);
    if (b.conditional) text += " (under a feature conditional)";
    if (!b.doc.empty()) text += "\n\n" + FirstParagraph(b.doc);
    return text;
}

std::string DescribeCl(const LispClDecl &d) {
    const std::string name = Lower(d.name);
    std::string text;
    const bool op = ClIsOperator(&d);
    if (op) {
        text = "(" + name + (d.lambda_list[0] != '\0' ? std::string(" ") + d.lambda_list : std::string()) + ")\n";
    } else {
        text = name + "\n";
    }
    std::string kinds = d.kinds;
    for (char &c : kinds) {
        if (c == ',') c = '/';
        if (c == '-') c = ' ';
    }
    text += "COMMON-LISP " + kinds;
    const LispLspVocabEntry *v = LispLspFindVocab(d.name);
    if (v != nullptr) text += "\n\n" + std::string(v->doc);
    if (d.doc[0] != '\0') text += std::string(v != nullptr ? "\n\nSBCL: " : "\n\n") + d.doc;
    return text;
}

/** @brief The innermost list node containing a position strictly inside its parens, or -1. */
int EnclosingList(const LispReadResult &r, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < r.nodes.size(); i++) {
        const LispNode &n = r.nodes[i];
        if (n.kind != LispNodeKind::List) continue;
        // Inside: after the open paren, and before the close paren (an
        // unclosed list extends to wherever the reader stopped).
        if (PosLess(line, col, n.range.line, n.range.col + 1)) continue;
        if (!PosLess(line, col, n.range.end_line, n.range.end_col)) {
            // Exactly at the end: only an unclosed list still contains it.
            const bool at_end = line == n.range.end_line && col == n.range.end_col;
            if (!at_end || n.range.end_col == 0) continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const LispNode &b = r.nodes[static_cast<size_t>(best)];
        if (PosLess(b.range.line, b.range.col, n.range.line, n.range.col)) best = static_cast<int>(i);
    }
    return best;
}

}  // namespace

// --- Hover -------------------------------------------------------------

LispLspHoverInfo LispLspHover(const std::vector<std::string> &lines, int line, int col) {
    LispLspHoverInfo out;
    const Index ix = BuildIndex(lines);
    const LispReadResult &r = ix.read;
    const int node = SymbolAt(r, line, col);
    if (node < 0) return out;
    const LispNode &n = r.at(node);
    out.range = n.range;
    // LOOP keywords are compared by name, in any package.
    if (n.parent >= 0) {
        const LispNode &p = r.at(n.parent);
        if (p.kind == LispNodeKind::List && !p.kids.empty() && p.kids[0] != node &&
            LispIsSymbolNamed(r.at(p.kids[0]), "LOOP")) {
            if (const LispLspVocabEntry *v = LispLspFindLoopKeyword(n.name)) {
                out.found = true;
                out.text = Lower(n.name) + " (loop keyword)\n" + v->detail + "\n\n" + v->doc;
                return out;
            }
        }
    }
    if (!n.name.empty() && n.name[0] == '&') {
        if (const LispLspVocabEntry *v = LispLspFindVocab(n.name)) {
            out.found = true;
            out.text = Lower(n.name) + " (lambda-list keyword)\n" + v->detail + "\n\n" + v->doc;
            return out;
        }
    }
    if (LispIsKeyword(n)) {
        out.found = true;
        out.text = ":" + Lower(n.name) + "\nkeyword: a symbol in the KEYWORD package, which evaluates to itself.";
        return out;
    }
    const int occ = OccAt(ix, node);
    int binding = occ >= 0 ? ix.occs[static_cast<size_t>(occ)].binding : -1;
    if (binding < 0 && occ >= 0 && ix.occs[static_cast<size_t>(occ)].ns == Ns::Data) binding = GlobalByName(ix, n.name);
    if (binding >= 0) {
        out.found = true;
        out.text = DescribeBinding(ix.bindings[static_cast<size_t>(binding)]);
        return out;
    }
    if (LispIsUnqualifiedOrCl(n)) {
        if (const LispClDecl *d = LispLspFindCl(n.name)) {
            out.found = true;
            out.text = DescribeCl(*d);
            return out;
        }
    }
    return out;
}

// --- Completion --------------------------------------------------------

namespace {

bool IsSymbolChar(char c) {
    return !(c == ' ' || c == '\t' || c == '(' || c == ')' || c == '\'' || c == '`' || c == ',' || c == '"' ||
             c == ';' || c == '#');
}

void AddItem(std::vector<LispLspCompletionItem> *out, std::set<std::string> *seen, const std::string &label,
             LispLspKind kind, const std::string &detail, const std::string &doc, int start, int end, bool upper) {
    if (!seen->insert(label).second) return;
    LispLspCompletionItem it;
    it.label = label;
    it.insert_text = upper ? Upper(label) : label;
    it.kind = kind;
    it.detail = detail;
    it.documentation = doc;
    it.replace_start = start;
    it.replace_end = end;
    out->push_back(it);
}

LispLspKind KindOfBinding(const Binding &b) {
    switch (b.kind) {
        case BK::Function:
        case BK::Macro:
        case BK::LocalFunction:
        case BK::LocalMacro:
        case BK::SetfFn:
        case BK::StructFn: return LispLspKind::Function;
        case BK::Generic:
        case BK::Accessor: return LispLspKind::Method;
        case BK::Constant: return LispLspKind::Constant;
        case BK::TypeName: return LispLspKind::Class;
        default: return LispLspKind::Variable;
    }
}

LispLspKind KindOfCl(const LispClDecl &d) {
    if (LispLspClHasKind(d, "special-operator") || LispLspClHasKind(d, "macro")) return LispLspKind::Keyword;
    if (LispLspClHasKind(d, "generic-function")) return LispLspKind::Method;
    if (LispLspClHasKind(d, "function")) return LispLspKind::Function;
    if (LispLspClHasKind(d, "constant")) return LispLspKind::Constant;
    if (LispLspClHasKind(d, "variable")) return LispLspKind::Variable;
    if (LispLspClHasKind(d, "class") || LispLspClHasKind(d, "type")) return LispLspKind::Class;
    return LispLspKind::Text;
}

/** @brief The operator of the call a position is inside, as a symbol node, or -1. */
int EnclosingOperator(const LispReadResult &r, int line, int col, int *list_out) {
    const int list = EnclosingList(r, line, col);
    if (list_out != nullptr) *list_out = list;
    if (list < 0) return -1;
    const LispNode &l = r.at(list);
    if (l.kids.empty() || r.at(l.kids[0]).kind != LispNodeKind::Symbol) return -1;
    return l.kids[0];
}

/** @brief The lambda-list text and shape of an operator, from the file or from CL. */
bool OperatorSignature(const Index &ix, int op_node, std::string *ll_text, std::string *doc) {
    const LispNode &op = ix.read.at(op_node);
    const int occ = OccAt(ix, op_node);
    const int binding = occ >= 0 ? ix.occs[static_cast<size_t>(occ)].binding : -1;
    if (binding >= 0) {
        const Binding &b = ix.bindings[static_cast<size_t>(binding)];
        if (b.ns == Ns::Func && !b.lambda_text.empty()) {
            std::string inner = b.lambda_text;
            if (inner.size() >= 2 && inner.front() == '(' && inner.back() == ')') {
                inner = inner.substr(1, inner.size() - 2);
            }
            *ll_text = inner;
            *doc = FirstParagraph(b.doc);
            return true;
        }
        return false;
    }
    if (!LispIsUnqualifiedOrCl(op)) return false;
    const LispClDecl *d = LispLspFindCl(op.name);
    if (d == nullptr || !ClIsOperator(d)) return false;
    *ll_text = d->lambda_list;
    const LispLspVocabEntry *v = LispLspFindVocab(d->name);
    *doc = v != nullptr ? v->doc : d->doc;
    return true;
}

}  // namespace

std::vector<LispLspCompletionItem> LispLspCompletions(const std::vector<std::string> &lines, int line, int col) {
    std::vector<LispLspCompletionItem> out;
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return out;
    const std::string &text = lines[static_cast<size_t>(line)];
    const int c = std::min(col, static_cast<int>(text.size()));
    int start = c;
    while (start > 0 && IsSymbolChar(text[static_cast<size_t>(start - 1)])) start--;
    std::string word = text.substr(static_cast<size_t>(start), static_cast<size_t>(c - start));
    std::string pkg;
    bool keyword = false;
    const size_t colon = word.rfind(':');
    if (colon != std::string::npos) {
        pkg = Upper(word.substr(0, colon));
        while (!pkg.empty() && pkg.back() == ':') pkg.pop_back();
        keyword = pkg.empty();
        start += static_cast<int>(colon) + 1;
        word = word.substr(colon + 1);
        if (!keyword && !IsClPackageName(pkg)) return out;  // another package's names are not known here
    }
    const std::string prefix = Upper(word);
    bool upper = false;
    for (char ch : word) {
        if (ch >= 'A' && ch <= 'Z') upper = true;
        if (ch >= 'a' && ch <= 'z') {
            upper = false;
            break;
        }
    }
    std::set<std::string> seen;
    const Index ix = BuildIndex(lines);
    const LispReadResult &r = ix.read;

    if (keyword) {
        // The enclosing call's keyword parameters first: that is nearly
        // always what a `:` typed inside a call is reaching for.
        const int op = EnclosingOperator(r, line, start - 1, nullptr);
        std::string ll;
        std::string doc;
        if (op >= 0 && OperatorSignature(ix, op, &ll, &doc)) {
            const LispLambdaShape shape = LispLspShapeOfLambdaList(ll);
            for (const std::string &k : shape.keys) {
                if (!StartsWith(k, prefix.c_str())) continue;
                AddItem(&out, &seen, Lower(k), LispLspKind::Keyword, "keyword argument of " + Lower(r.at(op).name), "",
                        start, c, upper);
            }
        }
        for (const LispNode &n : r.nodes) {
            if (!LispIsKeyword(n) || !StartsWith(n.name, prefix.c_str()) || n.name == prefix) continue;
            AddItem(&out, &seen, Lower(n.name), LispLspKind::Keyword, "keyword", "", start, c, upper);
        }
        return out;
    }

    if (pkg.empty()) {
        // Locals visible here: every scope whose form contains the cursor.
        for (size_t s = 1; s < ix.scopes.size(); s++) {
            const Scope &sc = ix.scopes[s];
            const bool inside = !PosLess(line, c, sc.range.line, sc.range.col) &&
                                (PosLess(line, c, sc.range.end_line, sc.range.end_col) ||
                                 (line == sc.range.end_line && c == sc.range.end_col));
            if (!inside) continue;
            for (int id : sc.bindings) {
                const Binding &b = ix.bindings[static_cast<size_t>(id)];
                if (b.ns != Ns::Var && b.ns != Ns::Func) continue;
                if (!StartsWith(b.name, prefix.c_str())) continue;
                if (b.range.line == line && b.range.col <= c && c <= b.range.end_col) continue;  // the word itself
                AddItem(&out, &seen, Lower(b.name), KindOfBinding(b), BindingTitle(b), "", start, c, upper);
            }
        }
        for (int id : ix.scopes[0].bindings) {
            const Binding &b = ix.bindings[static_cast<size_t>(id)];
            if (!StartsWith(b.name, prefix.c_str()) || b.name.find('(') != std::string::npos) continue;
            if (b.range.line == line && b.range.col <= c && c <= b.range.end_col) continue;
            AddItem(&out, &seen, Lower(b.name), KindOfBinding(b), BindingTitle(b), FirstParagraph(b.doc), start, c,
                    upper);
        }
    }
    if (prefix.empty() && pkg.empty()) {
        // An empty prefix with no context would list all 978 symbols;
        // the operator position is the one place that is still useful.
        if (start == 0 || text[static_cast<size_t>(start - 1)] != '(') return out;
    }
    for (size_t i = 0; i < kLispClDeclCount; i++) {
        const LispClDecl &d = kLispClDecls[i];
        if (!StartsWith(d.name, prefix.c_str())) continue;
        std::string detail = d.kinds;
        if (ClIsOperator(&d) && d.lambda_list[0] != '\0') detail = std::string("(") + Lower(d.name) + " " + d.lambda_list + ")";
        const LispLspVocabEntry *v = LispLspFindVocab(d.name);
        AddItem(&out, &seen, Lower(d.name), KindOfCl(d), detail, v != nullptr ? v->doc : d.doc, start, c, upper);
    }
    return out;
}

// --- Document symbols --------------------------------------------------

namespace {

/** @brief A `;;;;` heading, or a `;;; --- Title ---` rule-banner, as a section title. */
bool SectionTitle(const std::string &comment, bool *rule_style, std::string *title) {
    size_t semis = 0;
    while (semis < comment.size() && comment[semis] == ';') semis++;
    if (semis < 3) return false;
    std::string body = comment.substr(semis);
    bool has_rule = false;
    std::string collapsed;
    int run = 0;
    for (char c : body) {
        if (c == '-' || c == '=' || c == '*' || c == '_' || c == '~') {
            if (++run >= 3) has_rule = true;
            continue;
        }
        if (run > 0 && run < 3) collapsed += std::string(static_cast<size_t>(run), '-');
        run = 0;
        collapsed += c;
    }
    const size_t b = collapsed.find_first_not_of(" \t");
    const size_t e = collapsed.find_last_not_of(" \t");
    *title = b == std::string::npos ? std::string() : collapsed.substr(b, e - b + 1);
    *rule_style = has_rule;
    if (semis == 3 && !has_rule) return false;
    if (title->find("-*-") != std::string::npos) return false;  // a modeline
    return true;
}

struct OutlineBuilder {
    const std::vector<std::string> &lines;
    const LispReadResult &r;
    std::vector<LispLspSymbol> items;

    void Add(const std::string &name, const std::string &detail, LispLspSymbolKind kind, const LispRange &range,
             const LispRange &sel, int parent) {
        LispLspSymbol s;
        s.name = name;
        s.detail = detail;
        s.kind = kind;
        s.range = range;
        s.sel_range = sel;
        s.parent = parent;
        items.push_back(s);
    }

    static std::string Head(const LispReadResult &rr, int form) {
        const LispNode &f = rr.at(form);
        if (f.kind != LispNodeKind::List || f.kids.empty()) return std::string();
        const LispNode &h = rr.at(f.kids[0]);
        if (h.kind != LispNodeKind::Symbol || !LispIsUnqualifiedOrCl(h)) return std::string();
        return h.name;
    }

    std::string Text(int node) const {
        const LispNode &n = r.at(node);
        if (n.kind == LispNodeKind::Symbol) return n.text;
        return SourceText(lines, n.range);
    }

    void Form(int form) {
        const LispNode &f = r.at(form);
        if (f.kind == LispNodeKind::Feature) {
            if (f.kids.size() >= 2 && FeatureStateOf(r, f) != FeatureState::Dead) Form(f.kids[1]);
            return;
        }
        const std::string op = Head(r, form);
        if (op.empty()) return;
        auto kid = [&](size_t i) { return i < f.kids.size() ? f.kids[i] : -1; };
        if (op == "PROGN" || op == "LOCALLY") {
            for (size_t i = 1; i < f.kids.size(); i++) Form(f.kids[i]);
            return;
        }
        if (op == "EVAL-WHEN" || op == "LET" || op == "LET*" || op == "FLET" || op == "LABELS" || op == "MACROLET" ||
            op == "SYMBOL-MACROLET") {
            for (size_t i = 2; i < f.kids.size(); i++) Form(f.kids[i]);
            return;
        }
        const int name = kid(1);
        const LispNode &nn = r.at(name);
        if (name < 0) return;
        LispRange sel = nn.range;
        std::string label = Text(name);
        if (op == "DEFUN" || op == "DEFMACRO" || op == "DEFGENERIC" || op == "DEFINE-COMPILER-MACRO" ||
            op == "DEFSETF" || op == "DEFINE-MODIFY-MACRO" || op == "DEFINE-SETF-EXPANDER") {
            const int ll = kid(2);
            std::string detail = op == "DEFUN" ? std::string() : Lower(op);
            const std::string llt = ll >= 0 && r.at(ll).kind == LispNodeKind::List ? Text(ll) : std::string();
            if (!llt.empty()) detail = detail.empty() ? llt : detail + " " + llt;
            Add(label, detail, op == "DEFGENERIC" ? LispLspSymbolKind::Interface : LispLspSymbolKind::Function,
                f.range, sel, -1);
            return;
        }
        if (op == "DEFMETHOD") {
            std::string detail;
            size_t i = 2;
            for (; i < f.kids.size() && r.at(f.kids[i]).kind != LispNodeKind::List; i++) detail += Text(f.kids[i]) + " ";
            if (i < f.kids.size()) {
                // The specializers, which are what tell methods apart.
                std::string specs;
                for (int p : r.at(f.kids[i]).kids) {
                    const LispNode &pn = r.at(p);
                    if (pn.kind == LispNodeKind::Symbol && !pn.name.empty() && pn.name[0] == '&') break;
                    if (!specs.empty()) specs += ' ';
                    specs += pn.kind == LispNodeKind::List && pn.kids.size() >= 2 ? Text(pn.kids[1]) : "t";
                }
                detail += "(" + specs + ")";
            }
            Add(label, detail, LispLspSymbolKind::Method, f.range, sel, -1);
            return;
        }
        if (op == "DEFVAR" || op == "DEFPARAMETER" || op == "DEFINE-SYMBOL-MACRO") {
            Add(label, Lower(op), LispLspSymbolKind::Variable, f.range, sel, -1);
            return;
        }
        if (op == "DEFCONSTANT") {
            Add(label, "defconstant", LispLspSymbolKind::Constant, f.range, sel, -1);
            return;
        }
        if (op == "DEFTYPE") {
            Add(label, "deftype", LispLspSymbolKind::TypeParameter, f.range, sel, -1);
            return;
        }
        if (op == "DEFPACKAGE" || op == "IN-PACKAGE") {
            Add(label, Lower(op), LispLspSymbolKind::Package, f.range, sel, -1);
            return;
        }
        if (op == "DEFCLASS" || op == "DEFINE-CONDITION") {
            const int supers = kid(2);
            Add(label, op == "DEFCLASS" ? (supers >= 0 ? Text(supers) : "") : "condition " + Text(supers),
                LispLspSymbolKind::Class, f.range, sel, -1);
            const int parent = static_cast<int>(items.size()) - 1;
            const int slots = kid(3);
            if (slots >= 0 && r.at(slots).kind == LispNodeKind::List) {
                for (int slot : r.at(slots).kids) {
                    const LispNode &sn = r.at(slot);
                    const int sname = sn.kind == LispNodeKind::List && !sn.kids.empty() ? sn.kids[0] : slot;
                    if (r.at(sname).kind != LispNodeKind::Symbol) continue;
                    Add(Text(sname), "slot", LispLspSymbolKind::Field, sn.range, r.at(sname).range, parent);
                }
            }
            return;
        }
        if (op == "DEFSTRUCT") {
            const int sname = nn.kind == LispNodeKind::List && !nn.kids.empty() ? nn.kids[0] : name;
            if (r.at(sname).kind != LispNodeKind::Symbol) return;
            Add(Text(sname), "defstruct", LispLspSymbolKind::Struct, f.range, r.at(sname).range, -1);
            const int parent = static_cast<int>(items.size()) - 1;
            for (size_t i = 2; i < f.kids.size(); i++) {
                const LispNode &sn = r.at(f.kids[i]);
                const int slot = sn.kind == LispNodeKind::List && !sn.kids.empty() ? sn.kids[0] : f.kids[i];
                if (r.at(slot).kind != LispNodeKind::Symbol) continue;
                Add(Text(slot), "slot", LispLspSymbolKind::Field, sn.range, r.at(slot).range, parent);
            }
            return;
        }
    }
};

}  // namespace

std::vector<LispLspSymbol> LispLspSymbols(const std::vector<std::string> &lines) {
    const LispReadResult r = LispRead(lines);
    OutlineBuilder b{lines, r, {}};
    // Sections: a run of `;;;;` lines (or one rule-banner `;;;` line) at
    // column 0 names the stretch of file up to the next one.
    struct Section {
        std::string title;
        LispRange range;
    };
    std::vector<Section> sections;
    int last_line = -10;
    for (const LispComment &c : r.comments) {
        if (c.block || c.range.col != 0) continue;
        bool rule = false;
        std::string title;
        if (!SectionTitle(c.text, &rule, &title)) continue;
        if (title.empty()) continue;  // a pure rule line
        if (c.range.line == last_line + 1 && !sections.empty() && !rule) {
            last_line = c.range.line;  // a continuation of the same heading block
            continue;
        }
        last_line = c.range.line;
        sections.push_back({title.size() > 80 ? title.substr(0, 80) : title, c.range});
    }
    for (int top : r.top) b.Form(top);

    std::vector<LispLspSymbol> merged;
    std::vector<int> remap(b.items.size(), -1);
    size_t si = 0;
    int current_section = -1;
    auto emit_section = [&](const Section &s) {
        LispLspSymbol sym;
        sym.name = s.title;
        sym.detail = "section";
        sym.kind = LispLspSymbolKind::Namespace;
        sym.range = s.range;
        sym.sel_range = s.range;
        current_section = static_cast<int>(merged.size());
        merged.push_back(sym);
    };
    for (size_t i = 0; i < b.items.size(); i++) {
        const LispLspSymbol &it = b.items[i];
        if (it.parent < 0) {
            while (si < sections.size() && PosLess(sections[si].range.line, sections[si].range.col, it.range.line,
                                                   it.range.col)) {
                emit_section(sections[si++]);
            }
        }
        LispLspSymbol sym = it;
        if (it.parent >= 0) {
            sym.parent = remap[static_cast<size_t>(it.parent)];
        } else {
            sym.parent = current_section;
        }
        remap[i] = static_cast<int>(merged.size());
        merged.push_back(sym);
    }
    while (si < sections.size()) emit_section(sections[si++]);
    // A section owns everything up to the next one.
    for (size_t i = 0; i < merged.size(); i++) {
        if (merged[i].kind != LispLspSymbolKind::Namespace) continue;
        int end_line = static_cast<int>(lines.size()) - 1;
        for (size_t j = i + 1; j < merged.size(); j++) {
            if (merged[j].kind == LispLspSymbolKind::Namespace) {
                end_line = merged[j].range.line - 1;
                break;
            }
        }
        merged[i].range.end_line = std::max(merged[i].range.end_line, end_line);
        merged[i].range.end_col = 0;
    }
    return merged;
}

// --- Folding -----------------------------------------------------------

std::vector<LispLspFold> LispLspFolds(const std::vector<std::string> &lines) {
    const LispReadResult r = LispRead(lines);
    std::vector<LispLspFold> out;
    std::set<int> starts;
    auto add = [&](int start, int end, const char *kind) {
        if (end <= start || !starts.insert(start).second) return;
        LispLspFold f;
        f.start_line = start;
        f.end_line = end;
        f.kind = kind;
        out.push_back(f);
    };
    // Outermost first, so a start line folds its biggest form.
    std::vector<const LispNode *> lists;
    for (const LispNode &n : r.nodes) {
        if (n.kind == LispNodeKind::List || n.kind == LispNodeKind::Vector) lists.push_back(&n);
    }
    std::stable_sort(lists.begin(), lists.end(), [](const LispNode *a, const LispNode *b) {
        if (a->range.line != b->range.line) return a->range.line < b->range.line;
        return a->range.end_line > b->range.end_line;
    });
    for (const LispNode *n : lists) add(n->range.line, n->range.end_line, "");
    // Comments: block comments, and runs of whole-line `;` comments.
    int run_start = -1;
    int run_end = -1;
    for (const LispComment &c : r.comments) {
        if (c.block) {
            add(c.range.line, c.range.end_line, "comment");
            continue;
        }
        const std::string &text = lines[static_cast<size_t>(c.range.line)];
        if (text.find_first_not_of(" \t") != static_cast<size_t>(c.range.col)) continue;  // a trailing comment
        if (c.range.line == run_end + 1 && run_start >= 0) {
            run_end = c.range.line;
            continue;
        }
        if (run_start >= 0) add(run_start, run_end, "comment");
        run_start = run_end = c.range.line;
    }
    if (run_start >= 0) add(run_start, run_end, "comment");
    std::stable_sort(out.begin(), out.end(), [](const LispLspFold &a, const LispLspFold &b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.end_line > b.end_line;
    });
    return out;
}

// --- Definition / references / rename --------------------------------

namespace {

/** @brief The binding the symbol at a position refers to (a quoted name falls back to the file definition). */
int BindingAtPos(const Index &ix, int line, int col, int *node_out) {
    const int node = SymbolAt(ix.read, line, col);
    if (node_out != nullptr) *node_out = node;
    if (node < 0) return -1;
    const int occ = OccAt(ix, node);
    if (occ < 0) return -1;
    const Occ &o = ix.occs[static_cast<size_t>(occ)];
    if (o.binding >= 0) return o.binding;
    if (o.ns == Ns::Data) return GlobalByName(ix, o.name);
    return -1;
}

std::vector<LispLspReference> CollectReferences(const Index &ix, int line, int col) {
    std::vector<LispLspReference> out;
    int node = -1;
    const int binding = BindingAtPos(ix, line, col, &node);
    if (node < 0) return out;
    const LispNode &n = ix.read.at(node);
    if (LispIsKeyword(n)) {
        // A keyword is the same object everywhere.
        for (const LispNode &other : ix.read.nodes) {
            if (LispIsKeyword(other) && other.name == n.name) out.push_back({NameRange(other), false, false});
        }
        return out;
    }
    std::set<std::pair<int, int>> seen;
    auto push = [&](const Occ &o) {
        if (!seen.insert({o.range.line, o.range.col}).second) return;
        out.push_back({o.range, o.write, o.def});
    };
    if (binding >= 0) {
        const Binding &b = ix.bindings[static_cast<size_t>(binding)];
        // Every binding of a global name (a defun's methods, a struct's
        // generated functions) shares its occurrences.
        for (const Occ &o : ix.occs) {
            if (o.binding == binding) {
                push(o);
            } else if (b.scope == 0 && o.binding < 0 && o.ns == Ns::Data && o.name == b.name) {
                push(o);
            } else if (b.scope == 0 && o.binding >= 0 && ix.bindings[static_cast<size_t>(o.binding)].scope == 0 &&
                       ix.bindings[static_cast<size_t>(o.binding)].name == b.name &&
                       ix.bindings[static_cast<size_t>(o.binding)].ns == b.ns) {
                push(o);
            }
        }
    } else {
        for (const Occ &o : ix.occs) {
            if (o.name != n.name) continue;
            if (o.binding >= 0 && ix.bindings[static_cast<size_t>(o.binding)].scope != 0) continue;
            if (!(o.package == n.package || (LispIsUnqualifiedOrCl(n) && (o.package.empty() || IsClPackageName(o.package)))))
                continue;
            push(o);
        }
    }
    std::sort(out.begin(), out.end(), [](const LispLspReference &a, const LispLspReference &b) {
        return PosLess(a.range.line, a.range.col, b.range.line, b.range.col);
    });
    return out;
}

}  // namespace

LispLspLocation LispLspDefinition(const std::vector<std::string> &lines, int line, int col) {
    LispLspLocation out;
    const Index ix = BuildIndex(lines);
    const int binding = BindingAtPos(ix, line, col, nullptr);
    if (binding < 0) return out;
    out.found = true;
    out.range = ix.bindings[static_cast<size_t>(binding)].range;
    return out;
}

std::vector<LispLspReference> LispLspReferences(const std::vector<std::string> &lines, int line, int col) {
    const Index ix = BuildIndex(lines);
    return CollectReferences(ix, line, col);
}

LispLspRenameTarget LispLspPrepareRename(const std::vector<std::string> &lines, int line, int col) {
    LispLspRenameTarget out;
    const Index ix = BuildIndex(lines);
    int node = -1;
    const int binding = BindingAtPos(ix, line, col, &node);
    if (binding < 0 || node < 0) return out;
    const Binding &b = ix.bindings[static_cast<size_t>(binding)];
    // Names this file spells only indirectly (a structure's generated
    // functions) or not as a plain symbol cannot be renamed by editing
    // occurrences.
    if (b.kind == BK::StructFn || b.name.find('(') != std::string::npos) return out;
    if (b.kind == BK::TypeName && b.origin == "defstruct") return out;
    const LispNode &n = ix.read.at(node);
    if (LispIsKeyword(n)) return out;
    out.found = true;
    out.range = NameRange(n);
    out.placeholder = SourceText(lines, out.range);
    return out;
}

std::vector<LispLspTextEdit> LispLspRename(const std::vector<std::string> &lines, int line, int col,
                                           const std::string &new_name, std::string *error) {
    std::vector<LispLspTextEdit> out;
    auto fail = [&](const std::string &why) {
        if (error != nullptr) *error = why;
        return std::vector<LispLspTextEdit>{};
    };
    const LispLspRenameTarget target = LispLspPrepareRename(lines, line, col);
    if (!target.found) return fail("Only local variables and names this file defines can be renamed.");
    const LispReadResult check = LispRead({new_name});
    if (!check.errors.empty() || check.top.size() != 1 || check.at(check.top[0]).kind != LispNodeKind::Symbol ||
        !check.at(check.top[0]).package.empty() || new_name.find_first_of(" \t\n") != std::string::npos) {
        return fail("`" + new_name + "` does not read as one plain symbol.");
    }
    const Index ix = BuildIndex(lines);
    for (const LispLspReference &ref : CollectReferences(ix, line, col)) out.push_back({ref.range, new_name});
    return out;
}

// --- Signature help ----------------------------------------------------

LispLspSignature LispLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    LispLspSignature out;
    const Index ix = BuildIndex(lines);
    const LispReadResult &r = ix.read;
    int list = -1;
    const int op = EnclosingOperator(r, line, col, &list);
    if (op < 0) return out;
    // On the operator itself there is no argument yet to describe.
    if (LispRangeContains(r.at(op).range, line, col)) return out;
    std::string ll;
    std::string doc;
    if (!OperatorSignature(ix, op, &ll, &doc)) return out;
    const std::string name = Lower(r.at(op).name);
    out.found = true;
    out.label = "(" + name + (ll.empty() ? "" : " " + ll) + ")";
    out.documentation = doc;

    // Parameters, and which part of the call each one fills.
    struct Param {
        std::string text;
        int kind;  // 0 required, 1 optional, 2 rest, 3 key
        std::string key;
    };
    std::vector<Param> params;
    int mode = 0;
    bool skip = false;
    for (const std::string &e : LispLspSplitLambdaList(ll)) {
        const std::string up = Upper(e);
        if (skip) {
            skip = false;
            continue;
        }
        if (!up.empty() && up[0] == '&') {
            if (up == "&OPTIONAL") mode = 1;
            else if (up == "&REST" || up == "&BODY") mode = 2;
            else if (up == "&KEY") mode = 3;
            else if (up == "&AUX") mode = 4;
            else if (up == "&WHOLE" || up == "&ENVIRONMENT") skip = true;
            continue;
        }
        if (mode == 4) continue;
        Param p;
        p.text = e;
        p.kind = mode;
        if (mode == 3) {
            const LispReadResult pr = LispRead({e});
            if (!pr.top.empty()) p.key = KeyNameOfSpec(pr, pr.top[0]);
        }
        params.push_back(p);
        out.params.push_back(e);
    }
    // Which argument the cursor is on.
    const std::vector<int> &kids = r.at(list).kids;
    int arg = 0;
    int cursor_arg = -1;
    for (size_t i = 1; i < kids.size(); i++) {
        const LispRange &kr = r.at(kids[i]).range;
        if (PosLess(line, col, kr.line, kr.col)) break;
        if (PosLess(line, col, kr.end_line, kr.end_col) || (line == kr.end_line && col == kr.end_col)) {
            cursor_arg = static_cast<int>(i);
            arg = static_cast<int>(i) - 1;
            break;
        }
        arg = static_cast<int>(i);
    }
    int required = 0;
    int optional = 0;
    for (const Param &p : params) {
        if (p.kind == 0) required++;
        if (p.kind == 1) optional++;
    }
    auto index_of = [&](int kind, int nth) {
        int seen_n = 0;
        for (size_t i = 0; i < params.size(); i++) {
            if (params[i].kind != kind) continue;
            if (seen_n++ == nth) return static_cast<int>(i);
        }
        return -1;
    };
    if (arg < required) {
        out.active_param = index_of(0, arg);
    } else if (arg < required + optional) {
        out.active_param = index_of(1, arg - required);
    } else {
        int key_param = -1;
        const int rel = arg - required - optional;
        // On a keyword (or the value after one): the &key parameter it names.
        int key_node = -1;
        if (rel % 2 == 0 && cursor_arg >= 0) key_node = kids[static_cast<size_t>(cursor_arg)];
        if (rel % 2 == 1) key_node = kids[static_cast<size_t>(arg)];
        if (key_node >= 0 && LispIsKeyword(r.at(key_node))) {
            for (size_t i = 0; i < params.size(); i++) {
                if (params[i].kind == 3 && !params[i].key.empty() && StartsWith(params[i].key, r.at(key_node).name.c_str())) {
                    key_param = static_cast<int>(i);
                    break;
                }
            }
        }
        out.active_param = key_param >= 0 ? key_param : index_of(2, 0);
    }
    return out;
}

// --- Formatting -------------------------------------------------------

std::vector<LispLspTextEdit> LispLspFormat(const std::vector<std::string> &lines) {
    std::string text;
    for (const std::string &l : lines) text += l + "\n";
    const lispfmt::Result r = lispfmt::Format(text);
    std::vector<LispLspTextEdit> out;
    if (!r.ok || r.text == text) return out;
    std::vector<std::string> got;
    size_t p = 0;
    while (p < r.text.size()) {
        const size_t nl = r.text.find('\n', p);
        got.push_back(r.text.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    // One edit, over the lines from the first that changed to the last.
    size_t head = 0;
    while (head < lines.size() && head < got.size() && lines[head] == got[head]) head++;
    size_t tail = 0;
    while (tail < lines.size() - head && tail < got.size() - head &&
           lines[lines.size() - 1 - tail] == got[got.size() - 1 - tail]) {
        tail++;
    }
    LispLspTextEdit e;
    e.range.line = static_cast<int>(head);
    e.range.col = 0;
    e.range.end_line = static_cast<int>(lines.size() - tail);
    e.range.end_col = 0;
    for (size_t k = head; k < got.size() - tail; k++) e.new_text += got[k] + "\n";
    out.push_back(e);
    return out;
}
