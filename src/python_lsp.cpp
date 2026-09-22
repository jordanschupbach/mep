// mep's own Python language server, analysis half (see python_lsp.h for
// the scope statement and position conventions; python_ast.h is the
// parser this answers from, python_lsp_server.cpp the JSON-RPC wire half
// that turns these functions into the `mep-python-lsp` binary).
//
// The shape of this file is one class and eight entry points. The class
// is Analyzer: it walks the syntax tree once and builds the two things
// every feature needs -- a tree of scopes with the bindings each one
// introduces, and a list of every name *use* with the binding it resolves
// to. Diagnostics, completion, hover, definition, references and
// signature help are then queries over that, not separate walks of the
// source.
//
// Two rules keep the diagnostics honest, and both are load-bearing:
//   1. Never report what a single file cannot know. There is no type
//      checking here, `import numpy` makes `numpy.anything` acceptable,
//      and one `from x import *` switches the undefined-name check off
//      for the whole file rather than guessing.
//   2. Prefer silence to a false positive. A linter that cries wolf gets
//      switched off, and then its true reports go unseen too -- so every
//      check below has an explicit list of the legal-but-suspicious
//      shapes it must stay quiet about, and python_lsp_test.cpp asserts
//      the silence as deliberately as it asserts the findings.

#include "python_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "python_ast.h"

namespace {

// --- Small helpers ----------------------------------------------------

/** @brief Reports whether a byte can be part of an identifier the completion client would replace. */
bool IsWordChar(char c) { return IsPythonIdentChar(static_cast<unsigned char>(c)); }

/** @brief Reports whether `pos` is inside the half-open span [start, end). */
bool PosInSpan(PyPos pos, PyPos start, PyPos end) {
    if (pos.line < start.line || pos.line > end.line) return false;
    if (pos.line == start.line && pos.col < start.col) return false;
    if (pos.line == end.line && pos.col >= end.col) return false;
    return true;
}

/** @brief Reports whether `pos` is inside [start, end], the end included (a cursor may sit just past a name). */
bool PosInSpanInclusive(PyPos pos, PyPos start, PyPos end) {
    if (pos.line < start.line || pos.line > end.line) return false;
    if (pos.line == start.line && pos.col < start.col) return false;
    if (pos.line == end.line && pos.col > end.col) return false;
    return true;
}

/** @brief Extracts the source text a node covers, joining a multi-line span into one line. */
std::string SliceText(const std::vector<std::string> &lines, PyPos start, PyPos end) {
    if (start.line < 0 || start.line >= static_cast<int>(lines.size())) return std::string();
    if (end.line >= static_cast<int>(lines.size())) end.line = static_cast<int>(lines.size()) - 1;
    if (end.line < start.line) return std::string();
    if (start.line == end.line) {
        const std::string &line = lines[static_cast<size_t>(start.line)];
        const size_t from = std::min(static_cast<size_t>(start.col), line.size());
        const size_t to = std::min(static_cast<size_t>(end.col), line.size());
        return to > from ? line.substr(from, to - from) : std::string();
    }
    std::string out;
    for (int li = start.line; li <= end.line; li++) {
        const std::string &line = lines[static_cast<size_t>(li)];
        size_t from = 0, to = line.size();
        if (li == start.line) from = std::min(static_cast<size_t>(start.col), line.size());
        if (li == end.line) to = std::min(static_cast<size_t>(end.col), line.size());
        std::string piece = to > from ? line.substr(from, to - from) : std::string();
        // Leading whitespace of a continuation line is layout, not text.
        size_t b = 0;
        while (b < piece.size() && (piece[b] == ' ' || piece[b] == '\t')) b++;
        piece = piece.substr(b);
        if (!piece.empty() && !out.empty() && out.back() != ' ' && out.back() != '(' && out.back() != '[') out += ' ';
        out += piece;
    }
    // Collapse the runs a joined multi-line expression leaves behind.
    std::string tidy;
    for (char c : out) {
        if (c == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
        tidy += c;
    }
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

/** @brief The first sentence or line of a docstring, trimmed for a one-line annotation. */
std::string DocSummary(const std::string &doc, size_t limit = 220) {
    std::string first;
    for (char c : doc) {
        if (c == '\n') {
            if (!first.empty()) break;
            continue;
        }
        first += c;
    }
    size_t b = 0;
    while (b < first.size() && (first[b] == ' ' || first[b] == '\t')) b++;
    first = first.substr(b);
    while (!first.empty() && (first.back() == ' ' || first.back() == '\t' || first.back() == '\r')) first.pop_back();
    if (first.size() > limit) first = first.substr(0, limit - 3) + "...";
    return first;
}

// --- Bindings and scopes ----------------------------------------------

enum class BindKind {
    Import,      // `import os`
    ImportFrom,  // `from os import path`
    Function,
    Class,
    Param,
    Variable,     // a plain assignment
    ForTarget,
    WithVar,
    ExceptName,
    CompTarget,   // a comprehension's own loop variable
    TypeAlias,
    MatchCapture,
};

struct Binding {
    std::string name;
    BindKind kind = BindKind::Variable;
    PyPos pos;         // the bound name's own position
    int end_col = 0;
    int scope = 0;
    const PyNode *node = nullptr;  // the def/class/alias/target that bound it
    bool used = false;
    bool annotation_only = false;  // `x: int` with no value: a declaration
    bool augmented = false;        // bound by `x += 1`, which also reads it
    bool unpacked = false;         // bound by tuple unpacking, where an unused half is normal
    bool conditional = false;      // bound inside an if/try, so a later rebinding is not a redefinition
    std::string detail;            // signature, import source, inferred type
    std::string doc;               // docstring summary
    std::string type_name;         // "str", "MyClass", "module:os.path", or ""
    std::string import_module;     // the module an Import/ImportFrom binding came from
    std::string import_symbol;     // the name inside that module, for `from x import y`
};

enum class ScopeKind { Module, Class, Function, Lambda, Comprehension };

struct Scope {
    ScopeKind kind = ScopeKind::Module;
    int parent = -1;
    const PyNode *node = nullptr;  // the def/class/lambda that owns it
    std::string name;
    PyPos start;
    PyPos end;
    std::vector<int> bindings;             // indexes into Analyzer::bindings_
    std::map<std::string, int> by_name;    // name -> last binding index in this scope
    std::set<std::string> global_names;    // `global x` / `nonlocal x` declarations
    std::vector<int> children;
};

struct Use {
    std::string name;
    PyPos pos;
    int end_col = 0;
    int scope = 0;
    int binding = -1;  // resolved binding, or -1
    // A name read out of a *string* annotation (`def f(x: "Node")`). It
    // counts for "this import is used" but never for "this name is
    // undefined": the string might not be a type at all -- `Literal["w"]`
    // is a value -- and guessing wrong there invents errors.
    bool soft = false;
};

// What a class body defines, gathered once so `self.` completion, hover
// on a method and go-to-definition of an attribute all answer from the
// same place.
struct ClassMember {
    std::string name;
    std::string detail;
    std::string doc;
    PyPos pos;
    int kind = 8;  // LSP SymbolKind: Field 8, Method 6, Property 7
};

struct ClassInfo {
    const PyNode *node = nullptr;
    std::string name;
    std::vector<std::string> bases;
    std::vector<ClassMember> members;
    int scope = -1;
};

// One attribute access seen anywhere in the file, kept for the fallback
// "offer every attribute name this file mentions" completion.
struct AttrUse {
    std::string owner_text;  // the receiver as written ("self", "os.path")
    std::string name;
    PyPos pos;
};

class Analyzer {
public:
    Analyzer(const std::vector<std::string> &lines, const PythonLspOptions &opts)
        : lines_(lines), opts_(opts), parse_(ParsePython(lines)) {
        scopes_.push_back(Scope{});
        scopes_[0].kind = ScopeKind::Module;
        scopes_[0].name = "<module>";
        scopes_[0].start = PyPos{0, 0};
        scopes_[0].end = PyPos{static_cast<int>(lines.size()) + 1, 0};
        scopes_[0].node = parse_.module.get();
        module_doc_ = DocstringOf(parse_.module->body);
        CollectSuite(parse_.module->body, 0);
        GatherClasses(parse_.module->body, 0, std::string());
        Resolve();
    }

    const PyParseResult &parse() const { return parse_; }
    const std::vector<Scope> &scopes() const { return scopes_; }
    const std::vector<Binding> &bindings() const { return bindings_; }
    const std::vector<Use> &uses() const { return uses_; }
    const std::vector<ClassInfo> &classes() const { return classes_; }
    const std::vector<AttrUse> &attr_uses() const { return attr_uses_; }
    const std::vector<std::string> &exported() const { return exported_; }
    bool star_import() const { return star_import_; }
    bool dynamic_namespace() const { return dynamic_namespace_; }
    const std::string &module_doc() const { return module_doc_; }
    const std::vector<std::string> &lines() const { return lines_; }
    const PythonLspOptions &opts() const { return opts_; }

    /** @brief The innermost scope whose span contains a position (always at least the module scope). */
    int ScopeAt(PyPos pos) const {
        // Scopes nest, so among those containing the position the
        // innermost is simply the one that starts last.
        int best = 0;
        PyPos best_start{-1, -1};
        for (size_t i = 1; i < scopes_.size(); i++) {
            const Scope &sc = scopes_[i];
            if (!PosInSpanInclusive(pos, sc.start, sc.end)) continue;
            if (sc.start.line > best_start.line ||
                (sc.start.line == best_start.line && sc.start.col >= best_start.col)) {
                best_start = sc.start;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    /**
     * @brief Resolves a name as it would be seen from a scope, honoring Python's class-scope rule.
     * @return the binding index, or -1 when nothing in the chain binds it
     */
    int ResolveFrom(int scope, const std::string &name, bool is_own_scope = true) const {
        for (int s = scope; s >= 0;) {
            const Scope &sc = scopes_[static_cast<size_t>(s)];
            // A class body's names are invisible to functions nested in
            // it -- `class C: x = 1` does not put `x` in a method's
            // scope chain. They are visible to the class body itself.
            const bool skip = sc.kind == ScopeKind::Class && !is_own_scope;
            if (!skip) {
                const auto it = sc.by_name.find(name);
                if (it != sc.by_name.end()) return it->second;
            }
            is_own_scope = false;
            s = sc.parent;
        }
        return -1;
    }

    /** @brief The class whose body encloses a scope, or nullptr (used for `self` and dunder completion). */
    const ClassInfo *EnclosingClass(int scope) const {
        for (int s = scope; s >= 0; s = scopes_[static_cast<size_t>(s)].parent) {
            for (const ClassInfo &ci : classes_) {
                if (ci.scope == s) return &ci;
            }
        }
        return nullptr;
    }

    /** @brief Looks up a class this file defines by name. */
    const ClassInfo *ClassNamed(const std::string &name) const {
        for (const ClassInfo &ci : classes_) {
            if (ci.name == name) return &ci;
        }
        return nullptr;
    }

private:
    const std::vector<std::string> &lines_;
    const PythonLspOptions &opts_;
    PyParseResult parse_;
    std::vector<Scope> scopes_;
    std::vector<Binding> bindings_;
    std::vector<Use> uses_;
    std::vector<ClassInfo> classes_;
    std::vector<AttrUse> attr_uses_;
    std::vector<std::string> exported_;  // the names `__all__` lists
    std::string module_doc_;
    bool star_import_ = false;
    bool dynamic_namespace_ = false;  // globals().update(...), module __getattr__, ...
    int conditional_depth_ = 0;

public:
    /** @brief A decorator's text without its `@`, which its node's span includes. */
    std::string DecoratorText(const PyNode *node) const {
        std::string text = TextOf(node);
        if (!text.empty() && text[0] == '@') text.erase(0, 1);
        size_t begin = text.find_first_not_of(" \t");
        return begin == std::string::npos ? std::string() : text.substr(begin);
    }

    /** @brief The source text of an expression node, as written. */
    std::string TextOf(const PyNode *node) const {
        if (node == nullptr) return std::string();
        return SliceText(lines_, node->start, node->end);
    }

    /** @brief Formats a `def`'s parameter list and return annotation the way a signature popup wants it. */
    std::string SignatureOf(const PyNode *fn) const {
        std::string out = "(";
        bool first = true;
        for (const PyNodePtr &p : fn->params) {
            if (!first) out += ", ";
            first = false;
            out += ParamText(p.get());
        }
        out += ")";
        if (fn->kind == PyNodeKind::FunctionDef && !fn->kids.empty()) out += " -> " + TextOf(fn->kids[0].get());
        return out;
    }

    /** @brief Formats one parameter: star, name, annotation and default, exactly as written. */
    std::string ParamText(const PyNode *p) const {
        if (p->str_value == "/") return "/";
        std::string out = p->str_value + p->name;
        if (!p->kids.empty() && p->kids[0]->kind != PyNodeKind::Placeholder) {
            out += ": " + TextOf(p->kids[0].get());
        }
        if (p->kids.size() > 1) out += "=" + TextOf(p->kids[1].get());
        return out;
    }

    /** @brief The docstring of a suite whose first statement is a bare string literal. */
    static std::string DocstringOf(const std::vector<PyNodePtr> &body) {
        if (body.empty()) return std::string();
        const PyNode *first = body[0].get();
        if (first->kind != PyNodeKind::ExprStmt || first->kids.empty()) return std::string();
        const PyNode *expr = first->kids[0].get();
        if (expr->kind != PyNodeKind::Str && expr->kind != PyNodeKind::FString) return std::string();
        return expr->str_value;
    }

private:
    /** @brief Creates a nested scope and returns its index. */
    int PushScope(ScopeKind kind, int parent, const PyNode *node, const std::string &name) {
        Scope scope;
        scope.kind = kind;
        scope.parent = parent;
        scope.node = node;
        scope.name = name;
        scope.start = node != nullptr ? node->start : PyPos{};
        scope.end = node != nullptr ? node->end : PyPos{};
        scopes_.push_back(scope);
        const int index = static_cast<int>(scopes_.size()) - 1;
        scopes_[static_cast<size_t>(parent)].children.push_back(index);
        return index;
    }

    /** @brief Records a binding, honoring a `global`/`nonlocal` declaration by binding in the outer scope instead. */
    int Bind(const std::string &name, BindKind kind, PyPos pos, int end_col, int scope, const PyNode *node) {
        if (name.empty()) return -1;
        int target = scope;
        if (scopes_[static_cast<size_t>(scope)].global_names.count(name) > 0) {
            // `global x` in a function: the assignment binds the module's
            // name, so that is where the binding belongs (and why it must
            // not be reported as an unused local).
            for (int s = scope; s >= 0; s = scopes_[static_cast<size_t>(s)].parent) {
                if (scopes_[static_cast<size_t>(s)].parent < 0 ||
                    scopes_[static_cast<size_t>(s)].global_names.count(name) == 0) {
                    target = s;
                    break;
                }
                target = scopes_[static_cast<size_t>(s)].parent;
            }
        }
        Binding b;
        b.name = name;
        b.kind = kind;
        b.pos = pos;
        b.end_col = end_col;
        b.scope = target;
        b.node = node;
        b.conditional = conditional_depth_ > 0;
        bindings_.push_back(b);
        const int index = static_cast<int>(bindings_.size()) - 1;
        Scope &sc = scopes_[static_cast<size_t>(target)];
        sc.bindings.push_back(index);
        sc.by_name[name] = index;
        return index;
    }

    /** @brief Records a name being read. */
    void AddUse(const std::string &name, PyPos pos, int end_col, int scope, bool soft = false) {
        Use u;
        u.name = name;
        u.pos = pos;
        u.end_col = end_col;
        u.scope = scope;
        u.soft = soft;
        uses_.push_back(u);
    }

    // --- Collection ---------------------------------------------------

    /** @brief Walks a suite, collecting the bindings and uses of every statement in it. */
    void CollectSuite(const std::vector<PyNodePtr> &body, int scope) {
        for (const PyNodePtr &stmt : body) CollectStmt(stmt.get(), scope);
    }

    /** @brief Walks a suite with the "this binding is conditional" flag raised (an if/try/loop branch). */
    void CollectBranch(const std::vector<PyNodePtr> &body, int scope) {
        conditional_depth_++;
        CollectSuite(body, scope);
        conditional_depth_--;
    }

    /** @brief Collects one statement: its uses, the names it binds, and any scope it opens. */
    void CollectStmt(const PyNode *n, int scope) {
        if (n == nullptr) return;
        switch (n->kind) {
            case PyNodeKind::FunctionDef: {
                for (const PyNodePtr &d : n->decorators) CollectExpr(d.get(), scope);
                // Annotations and defaults are evaluated where the `def`
                // is, not inside the function.
                for (const PyNodePtr &p : n->params) {
                    for (const PyNodePtr &k : p->kids) CollectExpr(k.get(), scope);
                    if (!p->kids.empty()) NoteForwardRefs(p->kids[0].get(), scope);
                }
                for (const PyNodePtr &k : n->kids) {
                    CollectExpr(k.get(), scope);  // return annotation
                    NoteForwardRefs(k.get(), scope);
                }
                const int index = Bind(n->name, BindKind::Function, n->name_pos,
                                       n->name_pos.col + static_cast<int>(n->name.size()), scope, n);
                if (index >= 0) {
                    bindings_[static_cast<size_t>(index)].detail = SignatureOf(n);
                    bindings_[static_cast<size_t>(index)].doc = DocstringOf(n->body);
                }
                const int inner = PushScope(ScopeKind::Function, scope, n, n->name);
                for (const PyNodePtr &p : n->params) {
                    if (p->name.empty()) continue;
                    const int pi = Bind(p->name, BindKind::Param, p->name_pos,
                                        p->name_pos.col + static_cast<int>(p->name.size()), inner, p.get());
                    if (pi >= 0 && !p->kids.empty() && p->kids[0]->kind != PyNodeKind::Placeholder) {
                        bindings_[static_cast<size_t>(pi)].detail = TextOf(p->kids[0].get());
                        bindings_[static_cast<size_t>(pi)].type_name = SimpleTypeName(p->kids[0].get());
                    }
                }
                CollectSuite(n->body, inner);
                return;
            }
            case PyNodeKind::ClassDef: {
                for (const PyNodePtr &d : n->decorators) CollectExpr(d.get(), scope);
                for (const PyNodePtr &b : n->kids) CollectExpr(b.get(), scope);
                const int index = Bind(n->name, BindKind::Class, n->name_pos,
                                       n->name_pos.col + static_cast<int>(n->name.size()), scope, n);
                if (index >= 0) {
                    std::string bases;
                    for (const PyNodePtr &b : n->kids) {
                        if (b->kind == PyNodeKind::Keyword) continue;
                        if (!bases.empty()) bases += ", ";
                        bases += TextOf(b.get());
                    }
                    bindings_[static_cast<size_t>(index)].detail = bases.empty() ? std::string() : "(" + bases + ")";
                    bindings_[static_cast<size_t>(index)].doc = DocstringOf(n->body);
                }
                const int inner = PushScope(ScopeKind::Class, scope, n, n->name);
                CollectSuite(n->body, inner);
                return;
            }
            case PyNodeKind::Assign: {
                for (const PyNodePtr &v : n->kids) CollectExpr(v.get(), scope);
                const PyNode *value = n->kids.empty() ? nullptr : n->kids[0].get();
                for (const PyNodePtr &t : n->targets) BindTarget(t.get(), scope, BindKind::Variable, value);
                NoteModuleConventions(n, scope);
                return;
            }
            case PyNodeKind::AugAssign: {
                for (const PyNodePtr &v : n->kids) CollectExpr(v.get(), scope);
                // `x += 1` reads x as well as writing it.
                for (const PyNodePtr &t : n->targets) {
                    if (t->kind == PyNodeKind::Name) {
                        AddUse(t->name, t->start, t->start.col + static_cast<int>(t->name.size()), scope);
                        const int index = Bind(t->name, BindKind::Variable, t->start,
                                               t->start.col + static_cast<int>(t->name.size()), scope, t.get());
                        if (index >= 0) bindings_[static_cast<size_t>(index)].augmented = true;
                    } else {
                        CollectExpr(t.get(), scope);
                    }
                }
                return;
            }
            case PyNodeKind::AnnAssign: {
                for (const PyNodePtr &v : n->kids) CollectExpr(v.get(), scope);
                if (!n->kids.empty()) NoteForwardRefs(n->kids[0].get(), scope);
                const PyNode *value = n->kids.size() > 1 ? n->kids[1].get() : nullptr;
                for (const PyNodePtr &t : n->targets) {
                    const int before = static_cast<int>(bindings_.size());
                    BindTarget(t.get(), scope, BindKind::Variable, value);
                    for (size_t i = static_cast<size_t>(before); i < bindings_.size(); i++) {
                        bindings_[i].annotation_only = value == nullptr;
                        if (!n->kids.empty()) {
                            bindings_[i].detail = TextOf(n->kids[0].get());
                            if (bindings_[i].type_name.empty()) {
                                bindings_[i].type_name = SimpleTypeName(n->kids[0].get());
                            }
                        }
                    }
                }
                return;
            }
            case PyNodeKind::For: {
                for (const PyNodePtr &it : n->kids) CollectExpr(it.get(), scope);
                for (const PyNodePtr &t : n->targets) BindTarget(t.get(), scope, BindKind::ForTarget, nullptr);
                CollectBranch(n->body, scope);
                CollectBranch(n->orelse, scope);
                return;
            }
            case PyNodeKind::While:
            case PyNodeKind::If: {
                for (const PyNodePtr &t : n->kids) CollectExpr(t.get(), scope);
                CollectBranch(n->body, scope);
                CollectBranch(n->orelse, scope);
                return;
            }
            case PyNodeKind::With: {
                for (const PyNodePtr &item : n->kids) {
                    for (const PyNodePtr &ctx : item->kids) CollectExpr(ctx.get(), scope);
                    for (const PyNodePtr &t : item->targets) {
                        BindTarget(t.get(), scope, BindKind::WithVar, item->kids.empty() ? nullptr : item->kids[0].get());
                    }
                }
                CollectSuite(n->body, scope);
                return;
            }
            case PyNodeKind::Try: {
                CollectBranch(n->body, scope);
                for (const PyNodePtr &h : n->handlers) {
                    for (const PyNodePtr &t : h->kids) CollectExpr(t.get(), scope);
                    if (!h->name.empty()) {
                        // The `as` name is unbound again at the end of the
                        // handler, but for our purposes it is an ordinary
                        // local that happens to be short-lived.
                        Bind(h->name, BindKind::ExceptName, h->start, h->start.col, scope, h.get());
                    }
                    CollectBranch(h->body, scope);
                }
                CollectBranch(n->orelse, scope);
                CollectBranch(n->finalbody, scope);
                return;
            }
            case PyNodeKind::Import: {
                for (const PyNodePtr &alias : n->kids) {
                    // `import os.path` binds `os`; `import os.path as p`
                    // binds `p` and nothing else.
                    const bool aliased = !alias->str_value.empty();
                    const std::string bound = aliased ? alias->str_value : alias->name.substr(0, alias->name.find('.'));
                    const int index = Bind(bound, BindKind::Import, alias->name_pos,
                                           alias->name_pos.col + static_cast<int>(bound.size()), scope, alias.get());
                    if (index >= 0) {
                        bindings_[static_cast<size_t>(index)].import_module = alias->name;
                        bindings_[static_cast<size_t>(index)].detail = "module " + alias->name;
                        bindings_[static_cast<size_t>(index)].type_name =
                            "module:" + (aliased ? alias->name : bound);
                    }
                }
                return;
            }
            case PyNodeKind::ImportFrom: {
                for (const PyNodePtr &alias : n->kids) {
                    if (alias->name == "*") {
                        star_import_ = true;
                        continue;
                    }
                    const std::string bound = alias->str_value.empty() ? alias->name : alias->str_value;
                    const int index = Bind(bound, BindKind::ImportFrom, alias->name_pos,
                                           alias->name_pos.col + static_cast<int>(alias->name.size()), scope,
                                           alias.get());
                    if (index >= 0) {
                        bindings_[static_cast<size_t>(index)].import_module = n->name;
                        bindings_[static_cast<size_t>(index)].import_symbol = alias->name;
                        bindings_[static_cast<size_t>(index)].detail = "from " + n->name + " import " + alias->name;
                        // `from os import path` binds a module too, when
                        // the server happens to know that one.
                        const std::string sub = n->name + "." + alias->name;
                        if (PythonLspModuleMembers(sub) != nullptr) {
                            bindings_[static_cast<size_t>(index)].type_name = "module:" + sub;
                        }
                    }
                }
                return;
            }
            case PyNodeKind::Global:
            case PyNodeKind::Nonlocal: {
                for (const PyNodePtr &name : n->kids) {
                    scopes_[static_cast<size_t>(scope)].global_names.insert(name->name);
                }
                return;
            }
            case PyNodeKind::Match: {
                for (const PyNodePtr &subject : n->kids) CollectExpr(subject.get(), scope);
                for (const PyNodePtr &case_node : n->body) {
                    for (const PyNodePtr &pat : case_node->kids) CollectExpr(pat.get(), scope);
                    for (const PyNodePtr &cap : case_node->targets) {
                        Bind(cap->name, BindKind::MatchCapture, cap->start,
                             cap->start.col + static_cast<int>(cap->name.size()), scope, cap.get());
                    }
                    CollectBranch(case_node->body, scope);
                }
                return;
            }
            case PyNodeKind::TypeAlias: {
                for (const PyNodePtr &v : n->kids) CollectExpr(v.get(), scope);
                Bind(n->name, BindKind::TypeAlias, n->start, n->start.col, scope, n);
                return;
            }
            case PyNodeKind::Delete: {
                for (const PyNodePtr &t : n->targets) CollectExpr(t.get(), scope);
                return;
            }
            default: break;
        }
        // Everything else contributes uses only.
        for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->decorators}) {
            for (const PyNodePtr &kid : *vec) CollectExpr(kid.get(), scope);
        }
        CollectSuite(n->body, scope);
        CollectSuite(n->orelse, scope);
        CollectSuite(n->finalbody, scope);
    }

    /** @brief Notices the module-level conventions that switch checks off: `__all__`, `globals()`, `__getattr__`. */
    void NoteModuleConventions(const PyNode *assign, int scope) {
        if (scope != 0 || assign->targets.empty()) return;
        const PyNode *target = assign->targets[0].get();
        if (target->kind != PyNodeKind::Name || target->name != "__all__") return;
        if (assign->kids.empty()) return;
        const PyNode *value = assign->kids[0].get();
        if (value->kind != PyNodeKind::List && value->kind != PyNodeKind::Tuple) return;
        for (const PyNodePtr &item : value->kids) {
            if (item->kind == PyNodeKind::Str) exported_.push_back(item->str_value);
        }
    }

    /** @brief Binds an assignment target, recursing through tuple/list unpacking. */
    void BindTarget(const PyNode *target, int scope, BindKind kind, const PyNode *value) {
        if (target == nullptr) return;
        switch (target->kind) {
            case PyNodeKind::Name: {
                const int index = Bind(target->name, kind, target->start,
                                       target->start.col + static_cast<int>(target->name.size()), scope, target);
                if (index >= 0 && value != nullptr) {
                    bindings_[static_cast<size_t>(index)].type_name = InferType(value, scope);
                }
                return;
            }
            case PyNodeKind::Tuple:
            case PyNodeKind::List:
            case PyNodeKind::Starred: {
                // `a, b = f()`: an unused half of an unpacking is
                // ordinary Python, not a forgotten variable, so the
                // unused-local check has to be able to tell them apart.
                const size_t before = bindings_.size();
                for (const PyNodePtr &item : target->kids) BindTarget(item.get(), scope, kind, nullptr);
                for (size_t i = before; i < bindings_.size(); i++) bindings_[i].unpacked = true;
                return;
            }
            default:
                // `obj.attr = v` and `d[k] = v` bind nothing; they read
                // the object being mutated.
                CollectExpr(target, scope);
                return;
        }
    }

    /** @brief Walks an expression, recording name uses and opening scopes for lambdas and comprehensions. */
    void CollectExpr(const PyNode *n, int scope) {
        if (n == nullptr) return;
        switch (n->kind) {
            case PyNodeKind::Name:
                AddUse(n->name, n->start, n->start.col + static_cast<int>(n->name.size()), scope);
                return;
            case PyNodeKind::Attribute: {
                CollectExpr(n->kids.empty() ? nullptr : n->kids[0].get(), scope);
                AttrUse au;
                au.owner_text = n->kids.empty() ? std::string() : TextOf(n->kids[0].get());
                au.name = n->name;
                au.pos = n->name_pos;
                attr_uses_.push_back(au);
                return;
            }
            case PyNodeKind::Lambda: {
                for (const PyNodePtr &p : n->params) {
                    for (const PyNodePtr &k : p->kids) CollectExpr(k.get(), scope);
                }
                const int inner = PushScope(ScopeKind::Lambda, scope, n, "<lambda>");
                for (const PyNodePtr &p : n->params) {
                    if (p->name.empty()) continue;
                    Bind(p->name, BindKind::Param, p->name_pos,
                         p->name_pos.col + static_cast<int>(p->name.size()), inner, p.get());
                }
                for (const PyNodePtr &k : n->kids) CollectExpr(k.get(), inner);
                return;
            }
            case PyNodeKind::ListComp:
            case PyNodeKind::SetComp:
            case PyNodeKind::DictComp:
            case PyNodeKind::GeneratorExp: {
                // A comprehension has its own scope: its loop variables
                // do not leak. The outermost iterable is the exception --
                // Python evaluates it in the *enclosing* scope, before
                // the comprehension's own scope exists -- and that
                // exception is load-bearing, not a nicety: inside a class
                // body it is the only way a comprehension can see a class
                // variable at all, so `IDS = [f.__name__ for f in FUNCS]`
                // is legal exactly because FUNCS is read outside.
                const int inner = PushScope(ScopeKind::Comprehension, scope, n, "<comprehension>");
                bool outermost = true;
                for (const PyNodePtr &kid : n->kids) {
                    if (kid->kind != PyNodeKind::Comprehension) continue;
                    if (outermost && !kid->kids.empty()) CollectExpr(kid->kids[0].get(), scope);
                    for (const PyNodePtr &t : kid->targets) BindTarget(t.get(), inner, BindKind::CompTarget, nullptr);
                    outermost = false;
                }
                outermost = true;
                for (const PyNodePtr &kid : n->kids) {
                    if (kid->kind != PyNodeKind::Comprehension) {
                        CollectExpr(kid.get(), inner);
                        continue;
                    }
                    for (size_t i = outermost ? 1 : 0; i < kid->kids.size(); i++) {
                        CollectExpr(kid->kids[i].get(), inner);
                    }
                    outermost = false;
                }
                return;
            }
            case PyNodeKind::NamedExpr: {
                for (const PyNodePtr &v : n->kids) CollectExpr(v.get(), scope);
                // A walrus inside a comprehension binds in the enclosing
                // function scope, which is the whole point of it.
                int target_scope = scope;
                while (scopes_[static_cast<size_t>(target_scope)].kind == ScopeKind::Comprehension &&
                       scopes_[static_cast<size_t>(target_scope)].parent >= 0) {
                    target_scope = scopes_[static_cast<size_t>(target_scope)].parent;
                }
                for (const PyNodePtr &t : n->targets) {
                    BindTarget(t.get(), target_scope, BindKind::Variable, n->kids.empty() ? nullptr : n->kids[0].get());
                }
                return;
            }
            case PyNodeKind::Call: {
                if (!n->kids.empty()) {
                    const PyNode *callee = n->kids[0].get();
                    // `globals().update(...)` and friends make the module
                    // namespace unknowable, so the undefined-name check
                    // has to stand down.
                    if (callee->kind == PyNodeKind::Name &&
                        (callee->name == "globals" || callee->name == "locals" || callee->name == "vars" ||
                         callee->name == "eval" || callee->name == "exec")) {
                        dynamic_namespace_ = true;
                    }
                }
                for (const PyNodePtr &kid : n->kids) CollectExpr(kid.get(), scope);
                return;
            }
            default: break;
        }
        for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->params}) {
            for (const PyNodePtr &kid : *vec) CollectExpr(kid.get(), scope);
        }
        CollectSuite(n->body, scope);
    }

    /** @brief Records the names a string annotation mentions as uses, so a forward-referenced import counts. */
    void NoteForwardRefs(const PyNode *ann, int scope) {
        if (ann == nullptr) return;
        if (ann->kind == PyNodeKind::Str) {
            // `def f(x: "Node")`: the quotes are there to defer the
            // lookup, not to stop it being one.
            const std::string &text = ann->str_value;
            size_t i = 0;
            while (i < text.size()) {
                if (!IsPythonIdentStart(static_cast<unsigned char>(text[i]))) {
                    i++;
                    continue;
                }
                const size_t begin = i;
                while (i < text.size() && IsPythonIdentChar(static_cast<unsigned char>(text[i]))) i++;
                const std::string name = text.substr(begin, i - begin);
                if (!IsPythonKeyword(name)) AddUse(name, ann->start, ann->end.col, scope, true);
                // Only the head of a dotted path is a name; `a.b` reads a.
                while (i < text.size() && (text[i] == '.' || IsPythonIdentChar(static_cast<unsigned char>(text[i])))) {
                    i++;
                }
            }
            return;
        }
        for (const std::vector<PyNodePtr> *vec : {&ann->kids, &ann->targets}) {
            for (const PyNodePtr &kid : *vec) NoteForwardRefs(kid.get(), scope);
        }
    }

public:
    /** @brief Names the type an annotation expression denotes, when it is a plain name this server knows. */
    std::string SimpleTypeName(const PyNode *ann) const {
        if (ann == nullptr) return std::string();
        if (ann->kind == PyNodeKind::Name) return ann->name;
        if (ann->kind == PyNodeKind::Str) return ann->str_value;  // a forward reference
        if (ann->kind == PyNodeKind::Subscript && !ann->kids.empty() && ann->kids[0]->kind == PyNodeKind::Name) {
            return ann->kids[0]->name;  // `list[int]` is still a list
        }
        return std::string();
    }

    /** @brief Infers the type of an assigned value, as far as a single file honestly can. */
    std::string InferType(const PyNode *value, int scope) const {
        if (value == nullptr) return std::string();
        switch (value->kind) {
            case PyNodeKind::Str: return "str";
            case PyNodeKind::FString: return "str";
            case PyNodeKind::List: return "list";
            case PyNodeKind::ListComp: return "list";
            case PyNodeKind::Dict: return "dict";
            case PyNodeKind::DictComp: return "dict";
            case PyNodeKind::Set: return "set";
            case PyNodeKind::SetComp: return "set";
            case PyNodeKind::Tuple: return "tuple";
            case PyNodeKind::Number:
                if (value->name.find('.') != std::string::npos || value->name.find('e') != std::string::npos ||
                    value->name.find('E') != std::string::npos) {
                    return value->name.back() == 'j' || value->name.back() == 'J' ? "complex" : "float";
                }
                return value->name.back() == 'j' || value->name.back() == 'J' ? "complex" : "int";
            case PyNodeKind::Constant:
                if (value->name == "True" || value->name == "False") return "bool";
                return std::string();
            case PyNodeKind::Name: {
                const int index = ResolveFrom(scope, value->name);
                if (index < 0) return std::string();
                const Binding &b = bindings_[static_cast<size_t>(index)];
                return b.type_name.rfind("module:", 0) == 0 ? b.type_name : std::string();
            }
            case PyNodeKind::Call: {
                if (value->kids.empty()) return std::string();
                const PyNode *callee = value->kids[0].get();
                if (callee->kind != PyNodeKind::Name) return std::string();
                // A call to a class defined in this file yields an
                // instance of it; a call to a builtin constructor yields
                // that builtin type. Anything else is unknown, and saying
                // so is better than guessing.
                if (PythonLspTypeMembers(callee->name) != nullptr) return callee->name;
                const int index = ResolveFrom(scope, callee->name);
                if (index >= 0 && bindings_[static_cast<size_t>(index)].kind == BindKind::Class) return callee->name;
                return std::string();
            }
            default: return std::string();
        }
    }

private:
    /** @brief Resolves every recorded use against the scope chain, marking the bindings it finds as used. */
    void Resolve() {
        for (Use &u : uses_) {
            const int index = ResolveFrom(u.scope, u.name);
            u.binding = index;
            if (index < 0) continue;
            // A read marks *every* binding of that name in the scope it
            // resolved to, not just the last one. Without this, the very
            // ordinary shape
            //     if cond: colno = 1
            //     else:    colno = f()
            //     use(colno)
            // would report the first branch's binding as unused: the read
            // can only resolve to one of them, but both are read at run
            // time, and a linter that says otherwise is wrong twice over.
            const int owner = bindings_[static_cast<size_t>(index)].scope;
            for (int other : scopes_[static_cast<size_t>(owner)].bindings) {
                if (bindings_[static_cast<size_t>(other)].name == u.name) {
                    bindings_[static_cast<size_t>(other)].used = true;
                }
            }
        }
        // A name `__all__` exports counts as used, however the module
        // itself spells it.
        for (const std::string &name : exported_) {
            const int index = ResolveFrom(0, name);
            if (index >= 0) bindings_[static_cast<size_t>(index)].used = true;
        }
    }

    /** @brief Builds the class table: every `class` in the file, with its methods, class vars and `self.x` fields. */
    void GatherClasses(const std::vector<PyNodePtr> &body, int scope, const std::string &prefix) {
        for (const PyNodePtr &stmt : body) {
            if (stmt->kind == PyNodeKind::ClassDef) {
                ClassInfo info;
                info.node = stmt.get();
                info.name = prefix.empty() ? stmt->name : prefix + "." + stmt->name;
                for (const PyNodePtr &b : stmt->kids) {
                    if (b->kind != PyNodeKind::Keyword) info.bases.push_back(TextOf(b.get()));
                }
                for (size_t i = 0; i < scopes_.size(); i++) {
                    if (scopes_[i].node == stmt.get()) {
                        info.scope = static_cast<int>(i);
                        break;
                    }
                }
                CollectClassMembers(stmt.get(), &info);
                classes_.push_back(info);
                GatherClasses(stmt->body, info.scope, info.name);
                continue;
            }
            GatherClasses(stmt->body, scope, prefix);
            GatherClasses(stmt->orelse, scope, prefix);
            GatherClasses(stmt->finalbody, scope, prefix);
            for (const PyNodePtr &h : stmt->handlers) GatherClasses(h->body, scope, prefix);
        }
    }

    /** @brief Collects one class's methods, class variables and the `self.x` fields its methods assign. */
    void CollectClassMembers(const PyNode *cls, ClassInfo *info) {
        for (const PyNodePtr &stmt : cls->body) {
            if (stmt->kind == PyNodeKind::FunctionDef) {
                ClassMember m;
                m.name = stmt->name;
                m.detail = SignatureOf(stmt.get());
                m.doc = DocstringOf(stmt->body);
                m.pos = stmt->name_pos;
                m.kind = 6;  // Method
                for (const PyNodePtr &d : stmt->decorators) {
                    const std::string text = DecoratorText(d.get());
                    if (text == "property" || text.find(".setter") != std::string::npos) m.kind = 7;  // Property
                }
                info->members.push_back(m);
                CollectSelfFields(stmt.get(), info);
                continue;
            }
            if (stmt->kind == PyNodeKind::Assign || stmt->kind == PyNodeKind::AnnAssign) {
                for (const PyNodePtr &t : stmt->targets) {
                    if (t->kind != PyNodeKind::Name) continue;
                    ClassMember m;
                    m.name = t->name;
                    m.pos = t->start;
                    m.kind = 8;  // Field
                    if (stmt->kind == PyNodeKind::AnnAssign && !stmt->kids.empty()) {
                        m.detail = TextOf(stmt->kids[0].get());
                    } else if (!stmt->kids.empty()) {
                        m.detail = InferType(stmt->kids[0].get(), info->scope < 0 ? 0 : info->scope);
                    }
                    info->members.push_back(m);
                }
            }
        }
    }

    /** @brief Records the `self.x = ...` assignments a method makes as fields of its class. */
    void CollectSelfFields(const PyNode *fn, ClassInfo *info) {
        const std::string self_name = fn->params.empty() ? std::string("self") : fn->params[0]->name;
        if (self_name.empty()) return;
        WalkSelfFields(fn->body, self_name, info);
    }

    /** @brief Walks a suite looking for `self.x = ...` (and `self.x: T = ...`) assignments. */
    void WalkSelfFields(const std::vector<PyNodePtr> &body, const std::string &self_name, ClassInfo *info) {
        for (const PyNodePtr &stmt : body) {
            if (stmt->kind == PyNodeKind::Assign || stmt->kind == PyNodeKind::AnnAssign ||
                stmt->kind == PyNodeKind::AugAssign) {
                for (const PyNodePtr &t : stmt->targets) {
                    if (t->kind != PyNodeKind::Attribute || t->kids.empty()) continue;
                    const PyNode *owner = t->kids[0].get();
                    if (owner->kind != PyNodeKind::Name || owner->name != self_name) continue;
                    bool known = false;
                    for (const ClassMember &m : info->members) known = known || m.name == t->name;
                    if (known) continue;
                    ClassMember m;
                    m.name = t->name;
                    m.pos = t->name_pos;
                    m.kind = 8;  // Field
                    if (stmt->kind == PyNodeKind::AnnAssign && !stmt->kids.empty()) {
                        m.detail = TextOf(stmt->kids[0].get());
                    } else if (stmt->kind == PyNodeKind::Assign && !stmt->kids.empty()) {
                        m.detail = InferType(stmt->kids[0].get(), info->scope < 0 ? 0 : info->scope);
                    }
                    info->members.push_back(m);
                }
            }
            WalkSelfFields(stmt->body, self_name, info);
            WalkSelfFields(stmt->orelse, self_name, info);
            WalkSelfFields(stmt->finalbody, self_name, info);
            for (const PyNodePtr &h : stmt->handlers) WalkSelfFields(h->body, self_name, info);
        }
    }
};

// --- Diagnostics ------------------------------------------------------

// The implicit names a module or class body has without binding them.
// Reading one is never an undefined name.
const std::set<std::string> &ImplicitNames() {
    static const std::set<std::string> kNames = {
        "__all__",   "__annotations__", "__build_class__", "__builtins__", "__class__",    "__debug__",
        "__dict__",  "__doc__",         "__file__",        "__import__",   "__loader__",   "__module__",
        "__name__",  "__package__",     "__path__",        "__qualname__", "__slots__",    "__spec__",
        "_",
    };
    return kNames;
}

// Statements that end a suite: anything after one of these is dead.
/** @brief Reports whether a statement transfers control away unconditionally. */
bool IsTerminator(const PyNode *n) {
    return n->kind == PyNodeKind::Return || n->kind == PyNodeKind::Raise || n->kind == PyNodeKind::Break ||
           n->kind == PyNodeKind::Continue;
}

/** @brief Reports whether an expression is a literal whose identity is not guaranteed (`is` against it is a bug). */
bool IsIdentityUnsafeLiteral(const PyNode *n) {
    switch (n->kind) {
        case PyNodeKind::Str:
        case PyNodeKind::FString:
        case PyNodeKind::Number:
        case PyNodeKind::List:
        case PyNodeKind::Dict:
        case PyNodeKind::Set:
            return true;
        case PyNodeKind::Tuple:
            return !n->kids.empty();
        default:
            return false;
    }
}

/** @brief Reports whether a default-argument expression allocates a container shared across every call. */
bool IsMutableDefault(const PyNode *n) {
    if (n->kind == PyNodeKind::List || n->kind == PyNodeKind::Dict || n->kind == PyNodeKind::Set ||
        n->kind == PyNodeKind::ListComp || n->kind == PyNodeKind::DictComp || n->kind == PyNodeKind::SetComp) {
        return true;
    }
    if (n->kind != PyNodeKind::Call || n->kids.empty()) return false;
    const PyNode *callee = n->kids[0].get();
    if (callee->kind != PyNodeKind::Name) return false;
    return callee->name == "list" || callee->name == "dict" || callee->name == "set" ||
           callee->name == "bytearray" || callee->name == "collections";
}

// Walks the tree once, emitting everything that is not a scope question
// (those are answered from the Analyzer's own tables, in
// PythonLspDiagnostics below).
class DiagWalker {
public:
    DiagWalker(const Analyzer &analyzer, std::vector<PythonLspDiagnostic> *out) : a_(analyzer), out_(out) {}

    /** @brief Walks the module, reporting every per-statement and per-expression check. */
    void Run() { WalkSuite(a_.parse().module->body); }

private:
    const Analyzer &a_;
    std::vector<PythonLspDiagnostic> *out_;
    int loop_depth_ = 0;
    int func_depth_ = 0;
    const PyNode *func_ = nullptr;      // innermost enclosing `def`
    const PyNode *class_ = nullptr;     // innermost enclosing `class`
    bool in_finally_ = false;
    bool in_expected_failure_ = false;  // inside `with pytest.raises(...)` and friends

    /** @brief Emits one diagnostic covering a node's own span (clamped to its first line). */
    void Report(const PyNode *n, PythonLspSeverity sev, const std::string &code, const std::string &message) {
        ReportAt(n->start, n->start.line == n->end.line ? n->end.col : n->start.col + 1, sev, code, message);
    }

    /** @brief Emits one diagnostic with an explicit span on a single line. */
    void ReportAt(PyPos start, int end_col, PythonLspSeverity sev, const std::string &code,
                  const std::string &message) {
        PythonLspDiagnostic d;
        d.line = start.line;
        d.col_start = start.col;
        d.col_end = std::max(end_col, start.col + 1);
        d.severity = sev;
        d.code = code;
        d.message = message;
        out_->push_back(d);
    }

    /** @brief Walks a suite, reporting unreachable code and recursing into each statement. */
    void WalkSuite(const std::vector<PyNodePtr> &body) {
        bool dead = false;
        for (const PyNodePtr &stmt : body) {
            if (dead) {
                Report(stmt.get(), PythonLspSeverity::Hint, "unreachable",
                       "Unreachable: the statement above always leaves this block");
                dead = false;  // one report per suite is the useful amount
            }
            WalkStmt(stmt.get());
            if (IsTerminator(stmt.get())) dead = true;
        }
    }

    /** @brief Walks one statement and everything under it. */
    void WalkStmt(const PyNode *n) {
        switch (n->kind) {
            case PyNodeKind::FunctionDef: WalkFunction(n); return;
            case PyNodeKind::ClassDef: {
                for (const PyNodePtr &d : n->decorators) WalkExpr(d.get());
                for (const PyNodePtr &b : n->kids) WalkExpr(b.get());
                const PyNode *outer_class = class_;
                const PyNode *outer_func = func_;
                const int outer_loops = loop_depth_;
                class_ = n;
                func_ = nullptr;
                loop_depth_ = 0;
                WalkSuite(n->body);
                class_ = outer_class;
                func_ = outer_func;
                loop_depth_ = outer_loops;
                return;
            }
            case PyNodeKind::Return: {
                if (func_depth_ == 0) {
                    Report(n, PythonLspSeverity::Error, "return-outside-function",
                           "'return' outside a function");
                } else if (in_finally_) {
                    Report(n, PythonLspSeverity::Warning, "return-in-finally",
                           "'return' in a 'finally' block swallows any exception still in flight");
                } else if (func_ != nullptr && func_->name == "__init__" && !n->kids.empty() &&
                           n->kids[0]->kind != PyNodeKind::Constant) {
                    Report(n, PythonLspSeverity::Warning, "init-returns-value",
                           "__init__ must return None; return the value from a classmethod or __new__ instead");
                }
                break;
            }
            case PyNodeKind::Break:
            case PyNodeKind::Continue:
                if (loop_depth_ == 0) {
                    Report(n, PythonLspSeverity::Error, "outside-loop",
                           std::string("'") + (n->kind == PyNodeKind::Break ? "break" : "continue") +
                               "' outside a loop");
                }
                break;
            case PyNodeKind::Nonlocal:
                if (func_depth_ == 0) {
                    Report(n, PythonLspSeverity::Error, "nonlocal-at-module",
                           "'nonlocal' is only meaningful inside a nested function");
                }
                break;
            case PyNodeKind::For:
            case PyNodeKind::While: {
                for (const PyNodePtr &k : n->kids) WalkExpr(k.get());
                for (const PyNodePtr &t : n->targets) WalkExpr(t.get());
                loop_depth_++;
                WalkSuite(n->body);
                loop_depth_--;
                WalkSuite(n->orelse);
                return;
            }
            case PyNodeKind::Try: {
                WalkSuite(n->body);
                std::vector<std::string> seen;
                for (const PyNodePtr &h : n->handlers) {
                    if (h->kids.empty()) {
                        Report(h.get(), PythonLspSeverity::Warning, "bare-except",
                               "Bare 'except:' also catches KeyboardInterrupt and SystemExit; "
                               "use 'except Exception:'");
                    } else {
                        const std::string text = a_.TextOf(h->kids[0].get());
                        if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
                            Report(h.get(), PythonLspSeverity::Warning, "duplicate-except",
                                   "'" + text + "' is already handled by an earlier 'except' clause");
                        }
                        seen.push_back(text);
                        for (const PyNodePtr &k : h->kids) WalkExpr(k.get());
                    }
                    WalkSuite(h->body);
                }
                WalkSuite(n->orelse);
                const bool outer_finally = in_finally_;
                in_finally_ = true;
                WalkSuite(n->finalbody);
                in_finally_ = outer_finally;
                return;
            }
            case PyNodeKind::Assert: {
                if (!n->kids.empty() && n->kids[0]->kind == PyNodeKind::Tuple && !n->kids[0]->kids.empty()) {
                    Report(n, PythonLspSeverity::Warning, "assert-tuple",
                           "Asserting a non-empty tuple is always true; drop the parentheses or use a comma");
                }
                break;
            }
            case PyNodeKind::Raise: {
                if (!n->kids.empty() && n->kids[0]->kind == PyNodeKind::Name &&
                    n->kids[0]->name == "NotImplemented") {
                    Report(n->kids[0].get(), PythonLspSeverity::Warning, "raise-not-implemented",
                           "NotImplemented is a return value, not an exception; raise NotImplementedError");
                }
                break;
            }
            case PyNodeKind::ExprStmt: {
                if (!n->kids.empty() && !in_expected_failure_) CheckNoEffect(n->kids[0].get());
                break;
            }
            case PyNodeKind::With: {
                // `with pytest.raises(TypeError): arr + ts` is a whole
                // test idiom built on an expression statement whose
                // *only* job is to blow up, so the no-effect check has to
                // recognize it rather than call every such test a bug.
                bool expects_failure = in_expected_failure_;
                for (const PyNodePtr &item : n->kids) {
                    const std::string text = item->kids.empty() ? std::string() : a_.TextOf(item->kids[0].get());
                    expects_failure = expects_failure || text.find("raises") != std::string::npos ||
                                      text.find("warns") != std::string::npos ||
                                      text.find("Raises") != std::string::npos ||
                                      text.find("Warns") != std::string::npos ||
                                      text.find("deprecated_call") != std::string::npos;
                    for (const PyNodePtr &ctx : item->kids) WalkExpr(ctx.get());
                    for (const PyNodePtr &t : item->targets) WalkExpr(t.get());
                }
                const bool outer = in_expected_failure_;
                in_expected_failure_ = expects_failure;
                WalkSuite(n->body);
                in_expected_failure_ = outer;
                return;
            }
            default: break;
        }
        for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->decorators}) {
            for (const PyNodePtr &kid : *vec) WalkExpr(kid.get());
        }
        WalkSuite(n->body);
        WalkSuite(n->orelse);
        WalkSuite(n->finalbody);
        for (const PyNodePtr &h : n->handlers) WalkSuite(h->body);
    }

    /** @brief Checks a `def`: its parameters, its `self`, and its body. */
    void WalkFunction(const PyNode *n) {
        for (const PyNodePtr &d : n->decorators) WalkExpr(d.get());
        std::set<std::string> seen;
        for (const PyNodePtr &p : n->params) {
            if (!p->name.empty() && !seen.insert(p->name).second) {
                ReportAt(p->name_pos, p->name_pos.col + static_cast<int>(p->name.size()),
                         PythonLspSeverity::Error, "duplicate-param",
                         "Duplicate parameter '" + p->name + "'");
            }
            if (p->kids.size() > 1 && IsMutableDefault(p->kids[1].get())) {
                Report(p->kids[1].get(), PythonLspSeverity::Warning, "mutable-default",
                       "A mutable default is created once and shared by every call; use None and build it in "
                       "the body");
            }
            for (const PyNodePtr &k : p->kids) WalkExpr(k.get());
        }
        CheckSelfParameter(n);
        const PyNode *outer_func = func_;
        const PyNode *outer_class = class_;
        const int outer_loops = loop_depth_;
        func_ = n;
        class_ = nullptr;
        loop_depth_ = 0;
        func_depth_++;
        WalkSuite(n->body);
        func_depth_--;
        loop_depth_ = outer_loops;
        class_ = outer_class;
        func_ = outer_func;
    }

    /** @brief Reports a method whose first parameter is not the instance (or the class, for a classmethod). */
    void CheckSelfParameter(const PyNode *fn) {
        if (class_ == nullptr || func_ != nullptr) return;  // not a method
        bool is_static = false;
        bool is_class_method = false;
        for (const PyNodePtr &d : fn->decorators) {
            const std::string text = a_.DecoratorText(d.get());
            if (text == "staticmethod") is_static = true;
            if (text == "classmethod") is_class_method = true;
        }
        if (is_static) return;
        // A metaclass's methods take `cls`, and so does __new__ and
        // __init_subclass__/__class_getitem__ (implicit classmethods).
        if (fn->name == "__new__" || fn->name == "__init_subclass__" || fn->name == "__class_getitem__") return;
        if (fn->params.empty()) {
            ReportAt(fn->name_pos, fn->name_pos.col + static_cast<int>(fn->name.size()), PythonLspSeverity::Warning,
                     "no-self", "Method '" + fn->name + "' takes no 'self' parameter; add one or mark it @staticmethod");
            return;
        }
        const std::string &first = fn->params[0]->name;
        // `def f(*args)` / `def f(**kw)` have no first positional
        // parameter to name, and `def f(/, ...)` cannot happen first.
        if (first.empty() || !fn->params[0]->str_value.empty()) return;
        const std::string want = is_class_method ? "cls" : "self";
        if (first == want || first == "_" + want || first == "mcs" || first == "mcls" || first == "metacls") return;
        if (!is_class_method && first == "cls") return;  // a metaclass method, most likely
        ReportAt(fn->params[0]->name_pos, fn->params[0]->name_pos.col + static_cast<int>(first.size()),
                 PythonLspSeverity::Hint, "self-name",
                 "By convention the first parameter of a " + std::string(is_class_method ? "classmethod" : "method") +
                     " is called '" + want + "'");
    }

    /** @brief Reports an expression statement that computes something and throws it away. */
    void CheckNoEffect(const PyNode *e) {
        switch (e->kind) {
            case PyNodeKind::Compare:
                Report(e, PythonLspSeverity::Warning, "no-effect",
                       "This comparison's result is discarded; did you mean '=' instead of '=='?");
                return;
            case PyNodeKind::Name:
            case PyNodeKind::Number:
            case PyNodeKind::BinOp:
            case PyNodeKind::BoolOp:
            case PyNodeKind::UnaryOp:
            case PyNodeKind::List:
            case PyNodeKind::Dict:
            case PyNodeKind::Set:
                Report(e, PythonLspSeverity::Hint, "no-effect", "This expression's result is discarded");
                return;
            default:
                return;  // a call, an await, a yield, `...`, a docstring: all have a point
        }
    }

    /** @brief Walks an expression, reporting the checks that are about expressions. */
    void WalkExpr(const PyNode *n) {
        if (n == nullptr) return;
        switch (n->kind) {
            case PyNodeKind::Compare: CheckComparison(n); break;
            case PyNodeKind::FString:
                if (n->kids.empty()) {
                    Report(n, PythonLspSeverity::Hint, "f-string-without-placeholders",
                           "This f-string has no '{...}' placeholders");
                }
                break;
            case PyNodeKind::Dict: CheckDuplicateKeys(n); break;
            case PyNodeKind::Lambda: {
                std::set<std::string> seen;
                for (const PyNodePtr &p : n->params) {
                    if (!p->name.empty() && !seen.insert(p->name).second) {
                        ReportAt(p->name_pos, p->name_pos.col + static_cast<int>(p->name.size()),
                                 PythonLspSeverity::Error, "duplicate-param",
                                 "Duplicate parameter '" + p->name + "'");
                    }
                }
                break;
            }
            case PyNodeKind::Await:
                if (func_ == nullptr || !func_->is_async) {
                    Report(n, PythonLspSeverity::Error, "await-outside-async",
                           "'await' is only allowed inside an 'async def'");
                }
                break;
            case PyNodeKind::Yield:
            case PyNodeKind::YieldFrom:
                if (func_depth_ == 0) {
                    Report(n, PythonLspSeverity::Error, "yield-outside-function", "'yield' outside a function");
                }
                break;
            default: break;
        }
        for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->params}) {
            for (const PyNodePtr &kid : *vec) WalkExpr(kid.get());
        }
        WalkSuite(n->body);
    }

    /** @brief Reports identity tests against literals and equality tests against singletons. */
    void CheckComparison(const PyNode *n) {
        std::vector<std::string> ops;
        std::string cur;
        for (char c : n->name) {
            if (c == ' ' && cur != "not" && cur != "is") {
                ops.push_back(cur);
                cur.clear();
                continue;
            }
            if (c == ' ') {
                cur += c;
                continue;
            }
            cur += c;
        }
        if (!cur.empty()) ops.push_back(cur);
        for (size_t i = 0; i < ops.size() && i + 1 < n->kids.size(); i++) {
            const PyNode *left = n->kids[i].get();
            const PyNode *right = n->kids[i + 1].get();
            const std::string &op = ops[i];
            if (op == "is" || op == "is not") {
                const PyNode *literal = IsIdentityUnsafeLiteral(right)
                                            ? right
                                            : (IsIdentityUnsafeLiteral(left) ? left : nullptr);
                if (literal != nullptr) {
                    Report(literal, PythonLspSeverity::Warning, "is-literal",
                           "'" + op + "' compares identity, which is not guaranteed for literals; use '" +
                               (op == "is" ? "==" : "!=") + "'");
                }
                continue;
            }
            if (op != "==" && op != "!=") continue;
            const PyNode *singleton = nullptr;
            if (right->kind == PyNodeKind::Constant) singleton = right;
            if (left->kind == PyNodeKind::Constant) singleton = left;
            if (singleton != nullptr && singleton->name != "...") {
                Report(singleton, PythonLspSeverity::Hint, "compare-to-singleton",
                       "Compare to " + singleton->name + " with '" + (op == "==" ? "is" : "is not") + "'");
                continue;
            }
            const bool type_call = left->kind == PyNodeKind::Call && !left->kids.empty() &&
                                   left->kids[0]->kind == PyNodeKind::Name && left->kids[0]->name == "type";
            if (type_call) {
                Report(left, PythonLspSeverity::Hint, "type-comparison",
                       "Comparing type() results ignores subclasses; use isinstance()");
            }
        }
    }

    /** @brief Reports a dict literal that gives the same constant key twice. */
    void CheckDuplicateKeys(const PyNode *n) {
        std::vector<std::string> seen;
        for (size_t i = 0; i + 1 < n->kids.size(); i += 2) {
            const PyNode *key = n->kids[i].get();
            if (key->kind != PyNodeKind::Str && key->kind != PyNodeKind::Number) continue;
            const std::string text = key->kind == PyNodeKind::Str ? "s:" + key->str_value : "n:" + key->name;
            if (std::find(seen.begin(), seen.end(), text) != seen.end()) {
                Report(key, PythonLspSeverity::Warning, "duplicate-key",
                       "Duplicate key in this dict literal; the later value wins");
            }
            seen.push_back(text);
        }
    }
};

/** @brief Reports whether a decorator makes a second `def` of the same name legitimate (@property setters, overloads). */
bool IsRedefinitionDecorator(const std::string &text) {
    return text.find(".setter") != std::string::npos || text.find(".getter") != std::string::npos ||
           text.find(".deleter") != std::string::npos || text.find(".register") != std::string::npos ||
           text.find("overload") != std::string::npos;
}

}  // namespace

std::vector<PythonLspDiagnostic> PythonLspDiagnostics(const std::vector<std::string> &lines,
                                                      const PythonLspOptions &opts) {
    const Analyzer analyzer(lines, opts);
    std::vector<PythonLspDiagnostic> out;

    // 1. Syntax, straight from the parser.
    for (const PySyntaxError &e : analyzer.parse().errors) {
        PythonLspDiagnostic d;
        d.line = e.start.line;
        d.col_start = e.start.col;
        d.col_end = e.end.line == e.start.line ? std::max(e.end.col, e.start.col + 1) : e.start.col + 1;
        d.severity = PythonLspSeverity::Error;
        d.code = e.code;
        d.message = e.message;
        out.push_back(d);
    }
    // A file with a syntax error has, by definition, statements this
    // server could not read -- and therefore bindings it does not know
    // about. The checks that answer "is this name bound anywhere" would
    // start inventing undefined names in exactly the moment (mid-edit)
    // when that is least welcome, so they stand down until the file
    // parses. The purely local checks below keep running.
    const bool scope_checks = analyzer.parse().errors.empty();

    // 2. Everything derived from the scope tree.
    if (scope_checks && !analyzer.star_import() && !analyzer.dynamic_namespace()) {
        for (const Use &u : analyzer.uses()) {
            if (u.binding >= 0 || u.soft) continue;
            if (PythonLspIsBuiltin(u.name) || ImplicitNames().count(u.name) > 0) continue;
            PythonLspDiagnostic d;
            d.line = u.pos.line;
            d.col_start = u.pos.col;
            d.col_end = u.end_col;
            d.severity = PythonLspSeverity::Error;
            d.code = "undefined-name";
            d.message = "'" + u.name + "' is not defined in any enclosing scope";
            out.push_back(d);
        }
    }
    if (analyzer.star_import()) {
        for (const PyNodePtr &stmt : analyzer.parse().module->body) {
            if (stmt->kind != PyNodeKind::ImportFrom) continue;
            bool star = false;
            for (const PyNodePtr &alias : stmt->kids) star = star || alias->name == "*";
            if (!star) continue;
            PythonLspDiagnostic d;
            d.line = stmt->start.line;
            d.col_start = stmt->start.col;
            d.col_end = stmt->end.line == stmt->start.line ? stmt->end.col : stmt->start.col + 4;
            d.severity = PythonLspSeverity::Information;
            d.code = "star-import";
            d.message = "'from " + stmt->name +
                        " import *' hides what this module defines; undefined-name checking is off for this file";
            out.push_back(d);
        }
    }
    if (scope_checks) {
        const bool is_package_init = opts.file_name == "__init__.py";
        for (const Binding &b : analyzer.bindings()) {
            const bool is_import = b.kind == BindKind::Import || b.kind == BindKind::ImportFrom;
            const bool is_future = b.node != nullptr && b.node->kind == PyNodeKind::Alias &&
                                   b.detail.rfind("from __future__ ", 0) == 0;
            if (is_import && !b.used && !is_package_init && !is_future) {
                // `import x.y` for its side effects is a real idiom, but
                // so is forgetting to delete an import; the report is a
                // warning either way, never an error.
                PythonLspDiagnostic d;
                d.line = b.pos.line;
                d.col_start = b.pos.col;
                d.col_end = b.end_col;
                d.severity = PythonLspSeverity::Warning;
                d.code = "unused-import";
                d.message = "'" + b.name + "' is imported but never used";
                out.push_back(d);
            }
            const ScopeKind owner = analyzer.scopes()[static_cast<size_t>(b.scope)].kind;
            const bool local = owner == ScopeKind::Function || owner == ScopeKind::Lambda;
            if (local && b.kind == BindKind::Variable && !b.used && !b.augmented && !b.annotation_only &&
                !b.unpacked && !b.name.empty() && b.name[0] != '_') {
                PythonLspDiagnostic d;
                d.line = b.pos.line;
                d.col_start = b.pos.col;
                d.col_end = b.end_col;
                d.severity = PythonLspSeverity::Hint;
                d.code = "unused-variable";
                d.message = "Local variable '" + b.name + "' is assigned but never used";
                out.push_back(d);
            }
            if (b.name == "l" || b.name == "I" || b.name == "O") {
                PythonLspDiagnostic d;
                d.line = b.pos.line;
                d.col_start = b.pos.col;
                d.col_end = b.end_col;
                d.severity = PythonLspSeverity::Hint;
                d.code = "ambiguous-name";
                d.message = "'" + b.name + "' is easy to misread as a digit; pick a longer name";
                out.push_back(d);
            }
            const bool shadowable = (b.kind == BindKind::Variable || b.kind == BindKind::Function ||
                                     b.kind == BindKind::Class) &&
                                    owner != ScopeKind::Class;
            if (shadowable && PythonLspIsBuiltin(b.name) && !b.name.empty() && b.name[0] != '_') {
                PythonLspDiagnostic d;
                d.line = b.pos.line;
                d.col_start = b.pos.col;
                d.col_end = b.end_col;
                d.severity = PythonLspSeverity::Hint;
                d.code = "shadow-builtin";
                d.message = "'" + b.name + "' shadows the builtin of the same name in this scope";
                out.push_back(d);
            }
        }
        // Redefinition: a def/class/import replaced before anything read
        // it. Deliberately quiet about conditional bindings (the two
        // halves of a try/except import fallback) and about the decorator
        // idioms that redefine a name on purpose.
        for (const Scope &scope : analyzer.scopes()) {
            std::map<std::string, int> previous;
            for (int index : scope.bindings) {
                const Binding &b = analyzer.bindings()[static_cast<size_t>(index)];
                const auto it = previous.find(b.name);
                const int prev_index = it == previous.end() ? -1 : it->second;
                previous[b.name] = index;
                if (prev_index < 0) continue;
                const Binding &prev = analyzer.bindings()[static_cast<size_t>(prev_index)];
                const bool definitional = prev.kind == BindKind::Function || prev.kind == BindKind::Class ||
                                          prev.kind == BindKind::Import || prev.kind == BindKind::ImportFrom;
                if (!definitional || prev.conditional || b.conditional) continue;
                bool decorated = false;
                for (const Binding *side : {&prev, &b}) {
                    if (side->node == nullptr) continue;
                    for (const PyNodePtr &d : side->node->decorators) {
                        decorated = decorated || IsRedefinitionDecorator(analyzer.DecoratorText(d.get()));
                    }
                }
                if (decorated) continue;
                // `import os` then `import os.path`: the second is a
                // submodule import, which rebinds the same root name on
                // purpose and is not a mistake.
                if (prev.kind == BindKind::Import && b.kind == BindKind::Import && prev.node != nullptr &&
                    b.node != nullptr) {
                    const std::string &a_mod = prev.node->name;
                    const std::string &b_mod = b.node->name;
                    // A strict prefix means a submodule import; the
                    // *same* module twice is an ordinary duplicate.
                    if (a_mod != b_mod && (a_mod.rfind(b_mod + ".", 0) == 0 || b_mod.rfind(a_mod + ".", 0) == 0)) {
                        continue;
                    }
                }
                bool read_between = false;
                for (const Use &u : analyzer.uses()) {
                    if (u.name != b.name || u.binding < 0) continue;
                    if (analyzer.bindings()[static_cast<size_t>(u.binding)].scope != b.scope) continue;
                    const bool after_prev = u.pos.line > prev.pos.line ||
                                            (u.pos.line == prev.pos.line && u.pos.col >= prev.pos.col);
                    const bool before_now =
                        u.pos.line < b.pos.line || (u.pos.line == b.pos.line && u.pos.col < b.pos.col);
                    if (after_prev && before_now) {
                        read_between = true;
                        break;
                    }
                }
                if (read_between) continue;
                PythonLspDiagnostic d;
                d.line = b.pos.line;
                d.col_start = b.pos.col;
                d.col_end = b.end_col;
                d.severity = PythonLspSeverity::Warning;
                d.code = "redefinition";
                d.message = "Redefinition of '" + b.name + "' from line " + std::to_string(prev.pos.line + 1) +
                            ", which nothing used";
                out.push_back(d);
            }
        }
    }

    // 3. The per-statement and per-expression checks.
    DiagWalker walker(analyzer, &out);
    walker.Run();

    // 4. The one style check, and only when a caller asked for it.
    if (opts.max_line_length > 0) {
        for (size_t i = 0; i < lines.size(); i++) {
            const int width = static_cast<int>(lines[i].size());
            if (width <= opts.max_line_length) continue;
            PythonLspDiagnostic d;
            d.line = static_cast<int>(i);
            d.col_start = opts.max_line_length;
            d.col_end = width;
            d.severity = PythonLspSeverity::Hint;
            d.code = "line-too-long";
            d.message = "Line is " + std::to_string(width) + " characters, over the " +
                        std::to_string(opts.max_line_length) + "-column limit";
            out.push_back(d);
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const PythonLspDiagnostic &a, const PythonLspDiagnostic &b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col_start < b.col_start;
    });
    // The same finding can arrive from two directions (a use inside a
    // nested scope reported once per enclosing scope, say); one squiggle
    // per (position, code) is what the reader wants.
    std::vector<PythonLspDiagnostic> unique;
    for (const PythonLspDiagnostic &d : out) {
        bool dup = false;
        for (const PythonLspDiagnostic &seen : unique) {
            dup = dup || (seen.line == d.line && seen.col_start == d.col_start && seen.code == d.code);
        }
        if (!dup) unique.push_back(d);
    }
    return unique;
}

namespace {

// --- Position lookups -------------------------------------------------

// What the cursor is on: the identifier itself, plus enough context to
// say what kind of identifier it is.
struct NameHit {
    bool found = false;
    const PyNode *node = nullptr;   // the Name/Attribute/def/class/param/alias node
    const PyNode *owner = nullptr;  // an attribute's receiver expression
    std::string name;
    PyPos pos;
    int end_col = 0;
    bool is_attribute = false;
    bool is_definition = false;  // the cursor is on the name in a `def`/`class`/parameter/import
};

/** @brief Updates `best` if this node's own name span contains the position (deepest match wins). */
void ConsiderName(const PyNode *n, PyPos pos, PyPos name_pos, const std::string &name, bool is_attribute,
                  bool is_definition, NameHit *best) {
    if (name.empty()) return;
    const PyPos end{name_pos.line, name_pos.col + static_cast<int>(name.size())};
    if (!PosInSpanInclusive(pos, name_pos, end)) return;
    best->found = true;
    best->node = n;
    best->owner = is_attribute && !n->kids.empty() ? n->kids[0].get() : nullptr;
    best->name = name;
    best->pos = name_pos;
    best->end_col = end.col;
    best->is_attribute = is_attribute;
    best->is_definition = is_definition;
}

/** @brief Walks the tree for the innermost identifier whose own span covers a position. */
void FindNameAt(const PyNode *n, PyPos pos, NameHit *best) {
    if (n == nullptr) return;
    switch (n->kind) {
        case PyNodeKind::Name: ConsiderName(n, pos, n->start, n->name, false, false, best); break;
        case PyNodeKind::Attribute: ConsiderName(n, pos, n->name_pos, n->name, true, false, best); break;
        case PyNodeKind::FunctionDef:
        case PyNodeKind::ClassDef: ConsiderName(n, pos, n->name_pos, n->name, false, true, best); break;
        case PyNodeKind::Param: ConsiderName(n, pos, n->name_pos, n->name, false, true, best); break;
        case PyNodeKind::Alias:
            ConsiderName(n, pos, n->name_pos, n->str_value.empty() ? n->name : n->str_value, false, true, best);
            break;
        default: break;
    }
    for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->body, &n->orelse, &n->finalbody,
                                              &n->handlers, &n->decorators, &n->params}) {
        for (const PyNodePtr &kid : *vec) FindNameAt(kid.get(), pos, best);
    }
}

/** @brief Reports whether a position falls inside one of an f-string's parsed `{...}` expressions. */
void FindFStringFields(const PyNode *n, PyPos pos, bool *inside);

/** @brief Reports whether a position sits inside a comment, or inside string text that is not an interpolation. */
bool InTextLiteral(const Analyzer &analyzer, PyPos pos) {
    for (const PyComment &c : analyzer.parse().comments) {
        if (c.line == pos.line && pos.col > c.col) return true;
    }
    for (const PyToken &t : analyzer.parse().tokens) {
        if (t.kind != PyTokKind::String) continue;
        if (!PosInSpan(pos, t.start, t.end)) continue;
        if (t.str_prefix.find('f') == std::string::npos && t.str_prefix.find('t') == std::string::npos) return true;
        // Inside an f-string, only the replacement fields are code. The
        // parser already found them, so ask it rather than re-scanning.
        bool in_field = false;
        FindFStringFields(analyzer.parse().module.get(), pos, &in_field);
        return !in_field;
    }
    return false;
}

void FindFStringFields(const PyNode *n, PyPos pos, bool *inside) {
    if (n == nullptr || *inside) return;
    if (n->kind == PyNodeKind::FString) {
        for (const PyNodePtr &kid : n->kids) {
            if (PosInSpanInclusive(pos, kid->start, kid->end)) *inside = true;
        }
    }
    for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->body, &n->orelse, &n->finalbody,
                                              &n->handlers, &n->decorators, &n->params}) {
        for (const PyNodePtr &kid : *vec) FindFStringFields(kid.get(), pos, inside);
    }
}

// --- Type resolution for attribute access -----------------------------

/** @brief Names the type of an expression well enough to list its attributes ("str", "MyClass", "module:os.path"). */
std::string TypeOfExpr(const Analyzer &analyzer, const PyNode *expr, int scope) {
    if (expr == nullptr) return std::string();
    if (expr->kind == PyNodeKind::Name) {
        const int index = analyzer.ResolveFrom(scope, expr->name);
        if (index >= 0) {
            const Binding &b = analyzer.bindings()[static_cast<size_t>(index)];
            if (!b.type_name.empty()) return b.type_name;
            if (b.kind == BindKind::Class) return b.name;
            // `self` (whatever it is called) inside a method is an
            // instance of the class the method belongs to.
            if (b.kind == BindKind::Param) {
                const ClassInfo *cls = analyzer.EnclosingClass(scope);
                if (cls != nullptr && analyzer.bindings()[static_cast<size_t>(index)].pos.line >= 0) {
                    const Scope &owner = analyzer.scopes()[static_cast<size_t>(b.scope)];
                    if (owner.node != nullptr && !owner.node->params.empty() &&
                        owner.node->params[0]->name == b.name) {
                        return cls->name;
                    }
                }
            }
        }
        if (PythonLspTypeMembers(expr->name) != nullptr) return expr->name;
        if (PythonLspModuleMembers(expr->name) != nullptr) return "module:" + expr->name;
        return std::string();
    }
    if (expr->kind == PyNodeKind::Attribute) {
        const std::string owner = TypeOfExpr(analyzer, expr->kids.empty() ? nullptr : expr->kids[0].get(), scope);
        if (owner.rfind("module:", 0) == 0) {
            const std::string sub = owner.substr(7) + "." + expr->name;
            if (PythonLspModuleMembers(sub) != nullptr) return "module:" + sub;
        }
        return std::string();
    }
    if (expr->kind == PyNodeKind::Call && !expr->kids.empty()) {
        const PyNode *callee = expr->kids[0].get();
        if (callee->kind == PyNodeKind::Name) {
            if (PythonLspTypeMembers(callee->name) != nullptr) return callee->name;
            const int index = analyzer.ResolveFrom(scope, callee->name);
            if (index >= 0 && analyzer.bindings()[static_cast<size_t>(index)].kind == BindKind::Class) {
                return callee->name;
            }
        }
        return std::string();
    }
    return analyzer.InferType(expr, scope);
}

/** @brief Collects a class's members and those of any base class this same file defines. */
void CollectInheritedMembers(const Analyzer &analyzer, const ClassInfo *cls, std::vector<ClassMember> *out,
                             int depth = 0) {
    if (cls == nullptr || depth > 8) return;
    for (const ClassMember &m : cls->members) {
        bool seen = false;
        for (const ClassMember &have : *out) seen = seen || have.name == m.name;
        if (!seen) out->push_back(m);
    }
    for (const std::string &base : cls->bases) {
        // Only a base defined in this same file can be followed; an
        // imported one is outside what a single file can know.
        const ClassInfo *parent = analyzer.ClassNamed(base);
        if (parent != nullptr && parent != cls) CollectInheritedMembers(analyzer, parent, out, depth + 1);
    }
}

/** @brief Finds the attribute expression whose dot sits at a position, so `os.<cursor>` can name its receiver. */
void FindAttributeEndingAt(const PyNode *n, PyPos dot, const PyNode **receiver) {
    if (n == nullptr) return;
    if (n->kind == PyNodeKind::Attribute && !n->kids.empty()) {
        const PyNode *owner = n->kids[0].get();
        if (owner->end.line == dot.line && owner->end.col == dot.col) *receiver = owner;
    }
    for (const std::vector<PyNodePtr> *vec : {&n->kids, &n->targets, &n->body, &n->orelse, &n->finalbody,
                                              &n->handlers, &n->decorators, &n->params}) {
        for (const PyNodePtr &kid : *vec) FindAttributeEndingAt(kid.get(), dot, receiver);
    }
}

// --- Completion -------------------------------------------------------

// What the cursor is completing, worked out from the line's text and the
// parse. Everything the candidate builders need is in here, so each of
// them is a plain "append items" function.
struct CompletionContext {
    std::string prefix;      // the identifier characters already typed
    int replace_start = 0;   // where the prefix starts, for the client's replace range
    int replace_end = 0;     // the cursor itself
    bool after_dot = false;
    const PyNode *receiver = nullptr;  // the expression before the dot
    bool import_module = false;        // `import <here>` / `from <here>`
    std::string from_module;           // `from x import <here>`
    bool from_import = false;
    bool after_raise = false;
    bool def_in_class = false;  // `def <here>` directly inside a class body
    bool decorator = false;
    int scope = 0;
};

/** @brief Adds one candidate, filtered against the typed prefix. */
void Offer(std::vector<PythonLspCompletionItem> *out, const CompletionContext &ctx, const std::string &label,
           PythonLspKind kind, const std::string &detail, const std::string &doc,
           const std::string &insert = std::string()) {
    if (!ctx.prefix.empty() && label.compare(0, ctx.prefix.size(), ctx.prefix) != 0) return;
    for (const PythonLspCompletionItem &have : *out) {
        if (have.label == label) return;  // the nearest scope's version wins
    }
    PythonLspCompletionItem item;
    item.label = label;
    item.insert_text = insert.empty() ? label : insert;
    item.kind = kind;
    item.detail = detail;
    item.documentation = doc;
    item.replace_start = ctx.replace_start;
    item.replace_end = ctx.replace_end;
    out->push_back(item);
}

/** @brief The completion kind a binding should show up as. */
PythonLspKind KindOfBinding(const Binding &b) {
    switch (b.kind) {
        case BindKind::Function: return PythonLspKind::Function;
        case BindKind::Class: return PythonLspKind::Class;
        case BindKind::Import:
        case BindKind::ImportFrom: return PythonLspKind::Module;
        case BindKind::Param: return PythonLspKind::Variable;
        case BindKind::TypeAlias: return PythonLspKind::TypeParameter;
        default: return PythonLspKind::Variable;
    }
}

/** @brief Offers the members of a type, module or class the receiver expression resolves to. */
void OfferAttributes(const Analyzer &analyzer, const CompletionContext &ctx,
                     std::vector<PythonLspCompletionItem> *out) {
    const std::string type = TypeOfExpr(analyzer, ctx.receiver, ctx.scope);
    if (type.rfind("module:", 0) == 0) {
        const std::vector<PythonLspVocabEntry> *members = PythonLspModuleMembers(type.substr(7));
        if (members != nullptr) {
            for (const PythonLspVocabEntry &e : *members) {
                const bool callable = e.detail[0] == '(';
                Offer(out, ctx, e.name, callable ? PythonLspKind::Function : PythonLspKind::Constant, e.detail, e.doc);
            }
            return;
        }
    }
    if (!type.empty() && type.rfind("module:", 0) != 0) {
        const ClassInfo *cls = analyzer.ClassNamed(type);
        if (cls != nullptr) {
            std::vector<ClassMember> members;
            CollectInheritedMembers(analyzer, cls, &members);
            for (const ClassMember &m : members) {
                Offer(out, ctx, m.name,
                      m.kind == 6 ? PythonLspKind::Method
                                  : (m.kind == 7 ? PythonLspKind::Property : PythonLspKind::Field),
                      m.detail, m.doc);
            }
            return;
        }
        const std::vector<PythonLspVocabEntry> *members = PythonLspTypeMembers(type);
        if (members != nullptr) {
            for (const PythonLspVocabEntry &e : *members) {
                Offer(out, ctx, e.name, PythonLspKind::Method, e.detail, e.doc);
            }
            return;
        }
    }
    // Nothing is known about the receiver, which is the common case for
    // anything that came from another module. Offering every attribute
    // name this file uses on *the same receiver text* is a poor model of
    // Python and a good model of what the author is about to type.
    const std::string owner_text = analyzer.TextOf(ctx.receiver);
    for (const AttrUse &au : analyzer.attr_uses()) {
        if (au.owner_text != owner_text) continue;
        Offer(out, ctx, au.name, PythonLspKind::Field, std::string(), std::string());
    }
    for (const AttrUse &au : analyzer.attr_uses()) {
        Offer(out, ctx, au.name, PythonLspKind::Field, std::string(), "Seen elsewhere in this file");
    }
}

/** @brief Offers every name visible from the cursor's scope, innermost scope first. */
void OfferScopeNames(const Analyzer &analyzer, const CompletionContext &ctx,
                     std::vector<PythonLspCompletionItem> *out) {
    bool own_scope = true;
    for (int s = ctx.scope; s >= 0; s = analyzer.scopes()[static_cast<size_t>(s)].parent) {
        const Scope &scope = analyzer.scopes()[static_cast<size_t>(s)];
        if (scope.kind == ScopeKind::Class && !own_scope) {
            own_scope = false;
            continue;  // a class body's names are not visible to nested functions
        }
        own_scope = false;
        for (const auto &entry : scope.by_name) {
            const Binding &b = analyzer.bindings()[static_cast<size_t>(entry.second)];
            Offer(out, ctx, b.name, KindOfBinding(b), b.detail, DocSummary(b.doc));
        }
    }
}

/** @brief Builds the completion context from the raw line text and the parse. */
CompletionContext BuildContext(const Analyzer &analyzer, int line, int col) {
    CompletionContext ctx;
    const std::vector<std::string> &lines = analyzer.lines();
    const std::string &text = line >= 0 && line < static_cast<int>(lines.size())
                                  ? lines[static_cast<size_t>(line)]
                                  : std::string();
    const int limit = std::min(col, static_cast<int>(text.size()));
    int start = limit;
    while (start > 0 && IsWordChar(text[static_cast<size_t>(start) - 1])) start--;
    ctx.prefix = text.substr(static_cast<size_t>(start), static_cast<size_t>(limit - start));
    ctx.replace_start = start;
    ctx.replace_end = limit;
    ctx.scope = analyzer.ScopeAt(PyPos{line, col});
    ctx.after_dot = start > 0 && text[static_cast<size_t>(start) - 1] == '.';
    if (ctx.after_dot) {
        // The receiver is whatever expression the parser attached the dot
        // to -- including the one it built while recovering from the
        // half-written `os.` the cursor is sitting in.
        NameHit hit;
        FindNameAt(analyzer.parse().module.get(), PyPos{line, start}, &hit);
        if (hit.found && hit.is_attribute) {
            ctx.receiver = hit.owner;
        } else {
            FindAttributeEndingAt(analyzer.parse().module.get(), PyPos{line, start - 1}, &ctx.receiver);
        }
    }
    // The statement forms that complete something other than a name.
    std::string head;
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
    head = text.substr(i, static_cast<size_t>(limit) > i ? static_cast<size_t>(limit) - i : 0);
    const auto word_at = [&head](size_t at) {
        size_t end = at;
        while (end < head.size() && IsWordChar(head[end])) end++;
        return head.substr(at, end - at);
    };
    const std::string first = word_at(0);
    if (first == "import" && !ctx.after_dot) {
        ctx.import_module = true;
    } else if (first == "from") {
        const size_t import_at = head.find(" import ");
        if (import_at == std::string::npos) {
            ctx.import_module = true;
        } else {
            ctx.from_import = true;
            size_t m = 4;
            while (m < head.size() && head[m] == ' ') m++;
            ctx.from_module = head.substr(m, import_at - m);
        }
    } else if (first == "raise" || first == "except") {
        ctx.after_raise = true;
    } else if (first == "def") {
        // The half-typed `def ` has already opened a scope of its own by
        // the time this runs, so the question is what encloses *that*.
        int enclosing = ctx.scope;
        const Scope &own = analyzer.scopes()[static_cast<size_t>(enclosing)];
        if (own.kind == ScopeKind::Function && own.node != nullptr && own.node->start.line == line) {
            enclosing = own.parent < 0 ? 0 : own.parent;
        }
        ctx.def_in_class = analyzer.scopes()[static_cast<size_t>(enclosing)].kind == ScopeKind::Class;
    } else if (!head.empty() && head[0] == '@') {
        ctx.decorator = true;
    }
    return ctx;
}

}  // namespace

std::vector<PythonLspCompletionItem> PythonLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                          const PythonLspOptions &opts) {
    std::vector<PythonLspCompletionItem> out;
    if (line < 0 || line >= static_cast<int>(lines.size())) return out;
    const Analyzer analyzer(lines, opts);
    // Prose is not code: a comment or the text part of a string gets no
    // completions at all, which is the difference between a server that
    // helps and one that fights the writer.
    if (InTextLiteral(analyzer, PyPos{line, col > 0 ? col - 1 : 0})) return out;
    const CompletionContext ctx = BuildContext(analyzer, line, col);

    if (ctx.after_dot) {
        OfferAttributes(analyzer, ctx, &out);
        return out;
    }
    if (ctx.import_module) {
        for (const PythonLspVocabEntry &e : PythonLspModuleVocab()) {
            Offer(&out, ctx, e.name, PythonLspKind::Module, e.detail, e.doc);
        }
        // Sibling modules of the file being edited: `import <tab>` in a
        // project should offer that project's own modules first of all.
        if (opts.check_files && !opts.doc_dir.empty()) {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(opts.doc_dir, ec)) {
                if (ec) break;
                const std::filesystem::path &path = entry.path();
                if (path.extension() == ".py" && path.filename() != opts.file_name) {
                    Offer(&out, ctx, path.stem().string(), PythonLspKind::Module, "module in this directory",
                          std::string());
                } else if (entry.is_directory(ec) && std::filesystem::exists(path / "__init__.py", ec)) {
                    Offer(&out, ctx, path.filename().string(), PythonLspKind::Module, "package in this directory",
                          std::string());
                }
            }
        }
        return out;
    }
    if (ctx.from_import) {
        const std::vector<PythonLspVocabEntry> *members = PythonLspModuleMembers(ctx.from_module);
        if (members != nullptr) {
            for (const PythonLspVocabEntry &e : *members) {
                const bool callable = e.detail[0] == '(';
                Offer(&out, ctx, e.name, callable ? PythonLspKind::Function : PythonLspKind::Constant, e.detail,
                      e.doc);
            }
        }
        Offer(&out, ctx, "*", PythonLspKind::Text, "everything", "Import every public name (rarely a good idea)");
        return out;
    }
    if (ctx.def_in_class) {
        for (const PythonLspVocabEntry &e : PythonLspDunderVocab()) {
            Offer(&out, ctx, e.name, PythonLspKind::Method, e.detail, e.doc, std::string(e.name) + e.detail);
        }
        return out;
    }
    if (ctx.decorator) {
        for (const char *name : {"property", "staticmethod", "classmethod", "dataclass", "abstractmethod",
                                 "functools.cache", "functools.wraps", "contextlib.contextmanager"}) {
            Offer(&out, ctx, name, PythonLspKind::Function, "decorator", std::string());
        }
        OfferScopeNames(analyzer, ctx, &out);
        return out;
    }
    if (ctx.after_raise) {
        for (const PythonLspVocabEntry &e : PythonLspExceptionVocab()) {
            Offer(&out, ctx, e.name, PythonLspKind::Class, e.detail, e.doc);
        }
    }
    OfferScopeNames(analyzer, ctx, &out);
    for (const PythonLspVocabEntry &e : PythonLspBuiltinVocab()) {
        const bool callable = e.detail[0] == '(';
        Offer(&out, ctx, e.name, callable ? PythonLspKind::Function : PythonLspKind::Constant, e.detail, e.doc);
    }
    for (const PythonLspVocabEntry &e : PythonLspExceptionVocab()) {
        Offer(&out, ctx, e.name, PythonLspKind::Class, "exception", e.doc);
    }
    for (const PythonLspVocabEntry &e : PythonLspKeywordVocab()) {
        Offer(&out, ctx, e.name, PythonLspKind::Keyword, e.detail, e.doc);
    }
    return out;
}

namespace {

// --- Hover ------------------------------------------------------------

/** @brief The vocabulary entry for `from <module> import <name>`, when this server knows that module. */
const PythonLspVocabEntry *VocabForImport(const Binding &b) {
    if (b.import_module.empty() || b.import_symbol.empty()) return nullptr;
    const std::vector<PythonLspVocabEntry> *members = PythonLspModuleMembers(b.import_module);
    if (members == nullptr) return nullptr;
    for (const PythonLspVocabEntry &e : *members) {
        if (b.import_symbol == e.name) return &e;
    }
    return nullptr;
}

/** @brief The hover text for a binding: its declaration line, then its docstring summary. */
std::string HoverForBinding(const Analyzer &analyzer, const Binding &b) {
    std::string head;
    switch (b.kind) {
        case BindKind::Function: {
            const bool is_async = b.node != nullptr && b.node->is_async;
            head = std::string(is_async ? "async def " : "def ") + b.name + b.detail;
            const Scope &owner = analyzer.scopes()[static_cast<size_t>(b.scope)];
            if (owner.kind == ScopeKind::Class) head += "\n\nMethod of " + owner.name;
            break;
        }
        case BindKind::Class: head = "class " + b.name + b.detail; break;
        case BindKind::Param:
            head = "parameter " + b.name + (b.detail.empty() ? std::string() : ": " + b.detail);
            break;
        case BindKind::Import: head = b.detail.empty() ? "import " + b.name : b.detail; break;
        case BindKind::ImportFrom: {
            head = b.detail;
            const PythonLspVocabEntry *entry = VocabForImport(b);
            if (entry != nullptr) {
                if (entry->detail[0] != '\0') head += "\n\n" + b.name + std::string(entry->detail);
                if (entry->doc[0] != '\0') head += "\n\n" + std::string(entry->doc);
            }
            break;
        }
        case BindKind::TypeAlias: head = "type " + b.name; break;
        case BindKind::ExceptName: head = b.name + ": the exception this handler caught"; break;
        case BindKind::ForTarget: head = b.name + ": loop variable"; break;
        case BindKind::CompTarget: head = b.name + ": comprehension variable"; break;
        case BindKind::WithVar: head = b.name + ": bound by 'with'"; break;
        case BindKind::MatchCapture: head = b.name + ": captured by a 'case' pattern"; break;
        default: {
            head = b.name;
            if (!b.detail.empty() && b.detail != b.type_name) {
                head += ": " + b.detail;
            } else if (!b.type_name.empty()) {
                head += ": " + b.type_name;
            }
            // The assignment itself is often the most informative thing
            // there is about a plain variable.
            if (b.node != nullptr) {
                const std::string source = analyzer.lines()[static_cast<size_t>(b.pos.line)];
                std::string trimmed = source;
                size_t begin = trimmed.find_first_not_of(" \t");
                if (begin != std::string::npos) trimmed = trimmed.substr(begin);
                if (trimmed.size() > 120) trimmed = trimmed.substr(0, 117) + "...";
                head += "\n\n" + trimmed;
            }
            break;
        }
    }
    const std::string doc = DocSummary(b.doc, 400);
    if (!doc.empty()) head += "\n\n" + doc;
    head += "\n\nDefined on line " + std::to_string(b.pos.line + 1);
    return head;
}

/** @brief Looks a name up in a vocabulary table and formats it as hover text. */
bool HoverFromVocab(const std::vector<PythonLspVocabEntry> &table, const std::string &name, const char *what,
                    std::string *out) {
    for (const PythonLspVocabEntry &e : table) {
        if (name != e.name) continue;
        std::string text = name;
        if (e.detail[0] != '\0') {
            text += e.detail[0] == '(' ? std::string(e.detail) : std::string("  ") + e.detail;
        }
        text += std::string("\n\n") + what;
        if (e.doc[0] != '\0') text += std::string("\n\n") + e.doc;
        *out = text;
        return true;
    }
    return false;
}

}  // namespace

PythonLspHoverInfo PythonLspHover(const std::vector<std::string> &lines, int line, int col) {
    PythonLspHoverInfo info;
    PythonLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    const PyPos pos{line, col};
    if (InTextLiteral(analyzer, pos)) return info;

    NameHit hit;
    FindNameAt(analyzer.parse().module.get(), pos, &hit);
    if (!hit.found) {
        // Not on a name: a keyword still has something to say, and that
        // is the one place a beginner most wants it said.
        for (const PyToken &t : analyzer.parse().tokens) {
            if (t.kind != PyTokKind::Keyword || !PosInSpanInclusive(pos, t.start, t.end)) continue;
            std::string text;
            if (!HoverFromVocab(PythonLspKeywordVocab(), t.text, "Python keyword", &text)) return info;
            info.found = true;
            info.text = text;
            info.line = t.start.line;
            info.col_start = t.start.col;
            info.col_end = t.end.col;
            return info;
        }
        return info;
    }

    info.line = hit.pos.line;
    info.col_start = hit.pos.col;
    info.col_end = hit.end_col;
    const int scope = analyzer.ScopeAt(hit.pos);

    if (hit.is_attribute) {
        const std::string type = TypeOfExpr(analyzer, hit.owner, scope);
        if (type.rfind("module:", 0) == 0) {
            const std::vector<PythonLspVocabEntry> *members = PythonLspModuleMembers(type.substr(7));
            std::string text;
            if (members != nullptr && HoverFromVocab(*members, hit.name, ("Member of " + type.substr(7)).c_str(),
                                                     &text)) {
                info.found = true;
                info.text = text;
                return info;
            }
            const std::string sub = type.substr(7) + "." + hit.name;
            if (PythonLspModuleMembers(sub) != nullptr) {
                info.found = true;
                info.text = "module " + sub;
                return info;
            }
        }
        const ClassInfo *cls = analyzer.ClassNamed(type);
        if (cls != nullptr) {
            std::vector<ClassMember> members;
            CollectInheritedMembers(analyzer, cls, &members);
            for (const ClassMember &m : members) {
                if (m.name != hit.name) continue;
                info.found = true;
                info.text = (m.kind == 6 ? "def " : "") + m.name + m.detail + "\n\nMember of " + cls->name;
                const std::string doc = DocSummary(m.doc, 400);
                if (!doc.empty()) info.text += "\n\n" + doc;
                info.text += "\n\nDefined on line " + std::to_string(m.pos.line + 1);
                return info;
            }
        }
        const std::vector<PythonLspVocabEntry> *type_members = PythonLspTypeMembers(type);
        std::string text;
        if (type_members != nullptr && HoverFromVocab(*type_members, hit.name, ("Method of " + type).c_str(), &text)) {
            info.found = true;
            info.text = text;
            return info;
        }
        return info;  // an attribute of something unknown: say nothing rather than guess
    }

    const int index = analyzer.ResolveFrom(scope, hit.name);
    if (index >= 0) {
        info.found = true;
        info.text = HoverForBinding(analyzer, analyzer.bindings()[static_cast<size_t>(index)]);
        return info;
    }
    std::string text;
    if (HoverFromVocab(PythonLspBuiltinVocab(), hit.name, "Python builtin", &text) ||
        HoverFromVocab(PythonLspExceptionVocab(), hit.name, "Builtin exception", &text) ||
        HoverFromVocab(PythonLspKeywordVocab(), hit.name, "Python keyword", &text)) {
        info.found = true;
        info.text = text;
    }
    return info;
}

namespace {

// --- Import resolution ------------------------------------------------

/** @brief Resolves a module path to a file next to the document, following relative-import dots. */
std::string ResolveModuleFile(const PythonLspOptions &opts, const std::string &module) {
    if (!opts.check_files || opts.doc_dir.empty() || module.empty()) return std::string();
    std::error_code ec;
    std::filesystem::path base(opts.doc_dir);
    size_t i = 0;
    while (i < module.size() && module[i] == '.') {
        // One dot is "this package", each further dot climbs one level.
        if (i > 0) base = base.parent_path();
        i++;
    }
    std::string rest = module.substr(i);
    std::filesystem::path path = base;
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t dot = rest.find('.', start);
        const std::string part = rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty()) path /= part;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    const std::filesystem::path as_module = std::filesystem::path(path).concat(".py");
    if (std::filesystem::exists(as_module, ec) && !ec) return as_module.string();
    const std::filesystem::path as_package = path / "__init__.py";
    if (std::filesystem::exists(as_package, ec) && !ec) return as_package.string();
    return std::string();
}

/** @brief Finds where a file defines a name, so an import jumps to the definition rather than to line 1. */
int LineOfDefinition(const std::string &path, const std::string &name) {
    if (name.empty()) return 0;
    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        constexpr std::uintmax_t kMaxBytes = std::uintmax_t{4} * 1024 * 1024;
        if (ec || size > kMaxBytes) return 0;  // not worth reading a file that big to find a line
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<std::string> target;
    std::string current;
    while (std::getline(in, current)) {
        if (!current.empty() && current.back() == '\r') current.pop_back();
        target.push_back(current);
    }
    for (const PythonLspSymbol &s : PythonLspSymbols(target)) {
        if (s.parent < 0 && s.name == name) return s.sel_line;
    }
    return 0;
}

}  // namespace

PythonLspLocation PythonLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                      const PythonLspOptions &opts) {
    PythonLspLocation loc;
    const Analyzer analyzer(lines, opts);
    const PyPos pos{line, col};
    if (InTextLiteral(analyzer, pos)) return loc;
    NameHit hit;
    FindNameAt(analyzer.parse().module.get(), pos, &hit);
    if (!hit.found) return loc;
    const int scope = analyzer.ScopeAt(hit.pos);

    if (hit.is_attribute) {
        const std::string type = TypeOfExpr(analyzer, hit.owner, scope);
        const ClassInfo *cls = analyzer.ClassNamed(type);
        if (cls == nullptr) return loc;
        std::vector<ClassMember> members;
        CollectInheritedMembers(analyzer, cls, &members);
        for (const ClassMember &m : members) {
            if (m.name != hit.name) continue;
            loc.found = true;
            loc.line = m.pos.line;
            loc.col = m.pos.col;
            return loc;
        }
        return loc;
    }

    const int index = analyzer.ResolveFrom(scope, hit.name);
    if (index < 0) return loc;
    const Binding &b = analyzer.bindings()[static_cast<size_t>(index)];
    // An import is worth following out of the file when the module it
    // names is a sibling of this one; anything installed elsewhere is
    // outside what this server can see, and jumping to the import line
    // is then the honest answer.
    if ((b.kind == BindKind::Import || b.kind == BindKind::ImportFrom) && b.node != nullptr) {
        const std::string path = ResolveModuleFile(opts, b.import_module);
        const std::string symbol = b.import_symbol;
        if (!path.empty()) {
            loc.found = true;
            loc.path = path;
            loc.line = LineOfDefinition(path, symbol);
            loc.col = 0;
            return loc;
        }
    }
    loc.found = true;
    loc.line = b.pos.line;
    loc.col = b.pos.col;
    return loc;
}

PythonLspReferenceSet PythonLspReferences(const std::vector<std::string> &lines, int line, int col) {
    PythonLspReferenceSet out;
    PythonLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    const PyPos pos{line, col};
    if (InTextLiteral(analyzer, pos)) return out;
    NameHit hit;
    FindNameAt(analyzer.parse().module.get(), pos, &hit);
    if (!hit.found) return out;
    out.found = true;
    out.name = hit.name;

    if (hit.is_attribute) {
        // An attribute is not a binding, so "every use of this name as an
        // attribute, in this file" is the honest answer -- and the
        // caveat goes on the rename, which cannot see the other files
        // that might use it.
        const int scope = analyzer.ScopeAt(hit.pos);
        const std::string type = TypeOfExpr(analyzer, hit.owner, scope);
        const ClassInfo *cls = analyzer.ClassNamed(type);
        if (cls != nullptr) {
            for (const ClassMember &m : cls->members) {
                if (m.name != hit.name) continue;
                PythonLspReference ref;
                ref.line = m.pos.line;
                ref.col_start = m.pos.col;
                ref.col_end = m.pos.col + static_cast<int>(m.name.size());
                ref.is_write = true;
                out.refs.push_back(ref);
            }
        }
        for (const AttrUse &au : analyzer.attr_uses()) {
            if (au.name != hit.name) continue;
            PythonLspReference ref;
            ref.line = au.pos.line;
            ref.col_start = au.pos.col;
            ref.col_end = au.pos.col + static_cast<int>(au.name.size());
            out.refs.push_back(ref);
        }
        out.rename_blocked_reason =
            cls == nullptr ? "the owner of this attribute is not defined in this file" : std::string();
    } else {
        const int scope = analyzer.ScopeAt(hit.pos);
        const int index = analyzer.ResolveFrom(scope, hit.name);
        if (index < 0) {
            out.rename_blocked_reason = PythonLspIsBuiltin(hit.name) ? "'" + hit.name + "' is a Python builtin"
                                                                     : "'" + hit.name + "' is not defined here";
            PythonLspReference ref;
            ref.line = hit.pos.line;
            ref.col_start = hit.pos.col;
            ref.col_end = hit.end_col;
            out.refs.push_back(ref);
            return out;
        }
        const int owner_scope = analyzer.bindings()[static_cast<size_t>(index)].scope;
        for (const Binding &b : analyzer.bindings()) {
            if (b.scope != owner_scope || b.name != hit.name) continue;
            PythonLspReference ref;
            ref.line = b.pos.line;
            ref.col_start = b.pos.col;
            ref.col_end = b.end_col;
            ref.is_write = true;
            out.refs.push_back(ref);
        }
        for (const Use &u : analyzer.uses()) {
            if (u.binding < 0 || u.name != hit.name) continue;
            if (analyzer.bindings()[static_cast<size_t>(u.binding)].scope != owner_scope) continue;
            PythonLspReference ref;
            ref.line = u.pos.line;
            ref.col_start = u.pos.col;
            ref.col_end = u.end_col;
            out.refs.push_back(ref);
        }
    }
    std::stable_sort(out.refs.begin(), out.refs.end(),
                     [](const PythonLspReference &a, const PythonLspReference &b) {
                         if (a.line != b.line) return a.line < b.line;
                         return a.col_start < b.col_start;
                     });
    std::vector<PythonLspReference> unique;
    for (const PythonLspReference &r : out.refs) {
        if (!unique.empty() && unique.back().line == r.line && unique.back().col_start == r.col_start) {
            unique.back().is_write = unique.back().is_write || r.is_write;
            continue;
        }
        unique.push_back(r);
    }
    out.refs = std::move(unique);
    return out;
}

namespace {

// --- Symbols ----------------------------------------------------------

/** @brief Reports whether a name reads as a constant (SCREAMING_CASE), for SymbolKind.Constant. */
bool LooksLikeConstant(const std::string &name) {
    bool any_upper = false;
    for (char c : name) {
        if (std::islower(static_cast<unsigned char>(c)) != 0) return false;
        if (std::isupper(static_cast<unsigned char>(c)) != 0) any_upper = true;
    }
    return any_upper;
}

/** @brief Appends the symbols of one suite, linking each to its parent. */
void SymbolsOfSuite(const Analyzer &analyzer, const std::vector<PyNodePtr> &body, int parent, bool in_class,
                    bool allow_bindings, std::vector<PythonLspSymbol> *out) {
    for (const PyNodePtr &stmt : body) {
        if (stmt->kind == PyNodeKind::FunctionDef || stmt->kind == PyNodeKind::ClassDef) {
            PythonLspSymbol sym;
            sym.name = stmt->name;
            sym.kind = stmt->kind == PyNodeKind::ClassDef ? 5 : (in_class ? 6 : 12);
            sym.detail = stmt->kind == PyNodeKind::ClassDef ? std::string() : analyzer.SignatureOf(stmt.get());
            if (stmt->kind == PyNodeKind::ClassDef) {
                std::string bases;
                for (const PyNodePtr &b : stmt->kids) {
                    if (b->kind == PyNodeKind::Keyword) continue;
                    if (!bases.empty()) bases += ", ";
                    bases += analyzer.TextOf(b.get());
                }
                sym.detail = bases.empty() ? std::string() : "(" + bases + ")";
            } else if (stmt->is_async) {
                sym.detail = "async " + sym.detail;
            }
            sym.line_start = stmt->start.line;
            sym.line_end = stmt->end.line;
            sym.sel_line = stmt->name_pos.line;
            sym.sel_col_start = stmt->name_pos.col;
            sym.sel_col_end = stmt->name_pos.col + static_cast<int>(stmt->name.size());
            sym.parent = parent;
            out->push_back(sym);
            const int index = static_cast<int>(out->size()) - 1;
            SymbolsOfSuite(analyzer, stmt->body, index, stmt->kind == PyNodeKind::ClassDef,
                           stmt->kind == PyNodeKind::ClassDef, out);
            continue;
        }
        if ((stmt->kind == PyNodeKind::Assign || stmt->kind == PyNodeKind::AnnAssign) && allow_bindings) {
            for (const PyNodePtr &t : stmt->targets) {
                if (t->kind != PyNodeKind::Name) continue;
                PythonLspSymbol sym;
                sym.name = t->name;
                sym.kind = in_class ? 8 : (LooksLikeConstant(t->name) ? 14 : 13);
                if (stmt->kind == PyNodeKind::AnnAssign && !stmt->kids.empty()) {
                    sym.detail = analyzer.TextOf(stmt->kids[0].get());
                }
                sym.line_start = stmt->start.line;
                sym.line_end = stmt->end.line;
                sym.sel_line = t->start.line;
                sym.sel_col_start = t->start.col;
                sym.sel_col_end = t->start.col + static_cast<int>(t->name.size());
                sym.parent = parent;
                out->push_back(sym);
            }
            continue;
        }
        // A definition guarded by `if TYPE_CHECKING:` or wrapped in a
        // try/except is still a definition of this module.
        if (stmt->kind == PyNodeKind::If || stmt->kind == PyNodeKind::Try) {
            SymbolsOfSuite(analyzer, stmt->body, parent, in_class, allow_bindings, out);
            SymbolsOfSuite(analyzer, stmt->orelse, parent, in_class, allow_bindings, out);
            SymbolsOfSuite(analyzer, stmt->finalbody, parent, in_class, allow_bindings, out);
            for (const PyNodePtr &h : stmt->handlers) {
                SymbolsOfSuite(analyzer, h->body, parent, in_class, allow_bindings, out);
            }
        }
    }
}

}  // namespace

std::vector<PythonLspSymbol> PythonLspSymbols(const std::vector<std::string> &lines) {
    PythonLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    std::vector<PythonLspSymbol> out;
    SymbolsOfSuite(analyzer, analyzer.parse().module->body, -1, false, true, &out);
    return out;
}

namespace {

/** @brief Adds a fold for a suite, when it spans more than one line. */
void AddSuiteFold(int header_line, const std::vector<PyNodePtr> &body, std::vector<PythonLspFold> *out) {
    if (body.empty()) return;
    const int end = body.back()->end.line;
    if (end <= header_line) return;
    PythonLspFold fold;
    fold.start_line = header_line;
    fold.end_line = end;
    out->push_back(fold);
}

/** @brief Walks a suite, folding every compound statement and every multi-line string in it. */
void FoldSuite(const std::vector<PyNodePtr> &body, std::vector<PythonLspFold> *out) {
    for (const PyNodePtr &stmt : body) {
        switch (stmt->kind) {
            case PyNodeKind::FunctionDef:
            case PyNodeKind::ClassDef:
            case PyNodeKind::For:
            case PyNodeKind::While:
            case PyNodeKind::If:
            case PyNodeKind::With:
            case PyNodeKind::Match:
            case PyNodeKind::Try: {
                // The fold starts at the last line of the header (a
                // parameter list can span several), never before the
                // statement itself.
                const int header = stmt->body.empty() ? stmt->start.line : stmt->body.front()->start.line - 1;
                AddSuiteFold(std::max(stmt->start.line, header), stmt->body, out);
                FoldSuite(stmt->body, out);
                if (!stmt->orelse.empty()) {
                    AddSuiteFold(stmt->orelse.front()->start.line - 1, stmt->orelse, out);
                    FoldSuite(stmt->orelse, out);
                }
                if (!stmt->finalbody.empty()) {
                    AddSuiteFold(stmt->finalbody.front()->start.line - 1, stmt->finalbody, out);
                    FoldSuite(stmt->finalbody, out);
                }
                for (const PyNodePtr &h : stmt->handlers) {
                    AddSuiteFold(h->start.line, h->body, out);
                    FoldSuite(h->body, out);
                }
                if (stmt->kind == PyNodeKind::Match) {
                    for (const PyNodePtr &case_node : stmt->body) {
                        AddSuiteFold(case_node->start.line, case_node->body, out);
                        FoldSuite(case_node->body, out);
                    }
                }
                break;
            }
            default: break;
        }
    }
}

}  // namespace

std::vector<PythonLspFold> PythonLspFolds(const std::vector<std::string> &lines) {
    PythonLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    std::vector<PythonLspFold> out;
    FoldSuite(analyzer.parse().module->body, &out);

    // The import block at the top of the file: everything a reader scrolls
    // past on the way to the code.
    int import_start = -1, import_end = -1;
    for (const PyNodePtr &stmt : analyzer.parse().module->body) {
        if (stmt->kind == PyNodeKind::Import || stmt->kind == PyNodeKind::ImportFrom) {
            if (import_start < 0) import_start = stmt->start.line;
            import_end = stmt->end.line;
            continue;
        }
        if (import_start >= 0) break;
    }
    if (import_start >= 0 && import_end > import_start) {
        PythonLspFold fold;
        fold.start_line = import_start;
        fold.end_line = import_end;
        fold.kind = "imports";
        out.push_back(fold);
    }

    // Runs of two or more comment lines, and every multi-line string.
    const std::vector<PyComment> &comments = analyzer.parse().comments;
    for (size_t i = 0; i < comments.size();) {
        if (!comments[i].own_line) {
            i++;
            continue;
        }
        size_t j = i;
        while (j + 1 < comments.size() && comments[j + 1].own_line &&
               comments[j + 1].line == comments[j].line + 1) {
            j++;
        }
        if (j > i) {
            PythonLspFold fold;
            fold.start_line = comments[i].line;
            fold.end_line = comments[j].line;
            fold.kind = "comment";
            out.push_back(fold);
        }
        i = j + 1;
    }
    for (const PyToken &t : analyzer.parse().tokens) {
        if (t.kind != PyTokKind::String || t.end.line <= t.start.line) continue;
        PythonLspFold fold;
        fold.start_line = t.start.line;
        fold.end_line = t.end.line;
        out.push_back(fold);
    }

    std::stable_sort(out.begin(), out.end(), [](const PythonLspFold &a, const PythonLspFold &b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.end_line > b.end_line;
    });
    std::vector<PythonLspFold> unique;
    for (const PythonLspFold &f : out) {
        if (!unique.empty() && unique.back().start_line == f.start_line && unique.back().end_line == f.end_line) {
            continue;
        }
        unique.push_back(f);
    }
    return unique;
}

namespace {

// --- Signature help ---------------------------------------------------

// Where a call's argument list starts, found by scanning tokens
// backwards from the cursor. Lexical on purpose: signature help has to
// work on `f(` -- a call the grammar cannot finish parsing -- which is
// exactly the moment the user wants it.
struct CallSite {
    bool found = false;
    size_t open_paren = 0;   // index of the '(' token
    int active_param = 0;    // commas seen at depth 0 before the cursor
    std::string keyword;     // the `name=` the cursor is inside, if any
};

/** @brief Finds the innermost call whose argument list is still open at a position. */
CallSite FindCallSite(const std::vector<PyToken> &tokens, PyPos pos) {
    CallSite site;
    size_t cursor = 0;
    while (cursor < tokens.size() && (tokens[cursor].start.line < pos.line ||
                                      (tokens[cursor].start.line == pos.line && tokens[cursor].start.col < pos.col))) {
        cursor++;
    }
    int depth = 0;
    size_t i = cursor;
    while (i > 0) {
        i--;
        const PyToken &t = tokens[i];
        if (t.kind == PyTokKind::Newline || t.kind == PyTokKind::Indent || t.kind == PyTokKind::Dedent) {
            if (depth == 0) return site;  // a statement boundary with nothing open
            continue;
        }
        if (t.kind != PyTokKind::Op) continue;
        if (t.text == ")" || t.text == "]" || t.text == "}") {
            depth++;
            continue;
        }
        if (t.text == "[" || t.text == "{") {
            // An unmatched one means the cursor is inside a display or a
            // subscript -- `f([1, |])` -- so keep looking outward for the
            // call that contains *it*. Its own commas are not this
            // call's, which the argument-counting loop below handles by
            // depth.
            if (depth > 0) depth--;
            continue;
        }
        if (t.text != "(") continue;
        if (depth > 0) {
            depth--;
            continue;
        }
        // An open '(' with nothing between it and the cursor but a
        // balanced argument list: this is the call.
        if (i == 0) return site;
        const PyToken &before = tokens[i - 1];
        const bool is_call = before.kind == PyTokKind::Name ||
                             (before.kind == PyTokKind::Op && (before.text == ")" || before.text == "]"));
        if (!is_call) return site;
        site.found = true;
        site.open_paren = i;
        break;
    }
    if (!site.found) return site;
    int arg_depth = 0;
    for (size_t k = site.open_paren + 1; k < cursor; k++) {
        const PyToken &t = tokens[k];
        if (t.kind != PyTokKind::Op) continue;
        if (t.text == "(" || t.text == "[" || t.text == "{") arg_depth++;
        if (t.text == ")" || t.text == "]" || t.text == "}") arg_depth--;
        if (arg_depth != 0) continue;
        if (t.text == ",") {
            site.active_param++;
            site.keyword.clear();
        }
        if (t.text == "=" && k > site.open_paren + 1 && tokens[k - 1].kind == PyTokKind::Name) {
            site.keyword = tokens[k - 1].text;
        }
    }
    return site;
}

/** @brief Reads the dotted callee expression sitting immediately before a call's `(`. */
std::string CalleeText(const std::vector<PyToken> &tokens, size_t open_paren) {
    std::vector<std::string> parts;
    size_t i = open_paren;
    while (i > 0) {
        const PyToken &t = tokens[i - 1];
        if (t.kind == PyTokKind::Name && (parts.empty() || parts.back() == ".")) {
            parts.push_back(t.text);
            i--;
            continue;
        }
        if (t.kind == PyTokKind::Op && t.text == "." && !parts.empty() && parts.back() != ".") {
            parts.emplace_back(".");
            i--;
            continue;
        }
        break;
    }
    std::string out;
    for (size_t k = parts.size(); k-- > 0;) out += parts[k];
    return out;
}

/** @brief Splits a signature string like "(a, b=1, *args)" into its parameter labels. */
std::vector<std::string> SplitParams(const std::string &signature) {
    std::vector<std::string> params;
    size_t begin = signature.find('(');
    if (begin == std::string::npos) return params;
    int depth = 0;
    std::string current;
    for (size_t i = begin; i < signature.size(); i++) {
        const char c = signature[i];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            if (depth == 1) continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            depth--;
            if (depth == 0) break;
        }
        if (c == ',' && depth == 1) {
            params.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) params.push_back(current);
    for (std::string &p : params) {
        size_t b = p.find_first_not_of(" \t");
        size_t e = p.find_last_not_of(" \t");
        p = b == std::string::npos ? std::string() : p.substr(b, e - b + 1);
    }
    std::vector<std::string> tidy;
    for (const std::string &p : params) {
        if (!p.empty()) tidy.push_back(p);
    }
    return tidy;
}

}  // namespace

PythonLspSignature PythonLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    PythonLspSignature out;
    PythonLspOptions opts;
    opts.check_files = false;
    const Analyzer analyzer(lines, opts);
    const PyPos pos{line, col};
    // No InTextLiteral guard here, unlike every other feature: typing a
    // *string argument* is one of the moments signature help is most
    // wanted, and the backwards scan is over tokens, where a whole string
    // literal is one token and its contents cannot be mistaken for a
    // call.
    const CallSite site = FindCallSite(analyzer.parse().tokens, pos);
    if (!site.found) return out;
    const std::string callee = CalleeText(analyzer.parse().tokens, site.open_paren);
    if (callee.empty()) return out;
    const int scope = analyzer.ScopeAt(pos);

    const std::string head = callee.substr(0, callee.find('.'));
    const std::string tail = callee.find('.') == std::string::npos ? std::string()
                                                                   : callee.substr(callee.rfind('.') + 1);
    std::string label;
    std::string doc;
    bool drop_self = false;

    if (tail.empty()) {
        const int index = analyzer.ResolveFrom(scope, callee);
        if (index >= 0) {
            const Binding &b = analyzer.bindings()[static_cast<size_t>(index)];
            if (b.kind == BindKind::Function) {
                label = b.name + b.detail;
                doc = DocSummary(b.doc, 400);
            } else if (b.kind == BindKind::Class) {
                const ClassInfo *cls = analyzer.ClassNamed(b.name);
                std::vector<ClassMember> members;
                if (cls != nullptr) CollectInheritedMembers(analyzer, cls, &members);
                for (const ClassMember &m : members) {
                    if (m.name != "__init__") continue;
                    label = b.name + m.detail;
                    doc = DocSummary(m.doc, 400);
                    drop_self = true;
                }
                if (label.empty()) {
                    label = b.name + "()";
                    doc = DocSummary(b.doc, 400);
                }
            }
        }
        if (label.empty() && index >= 0) {
            const PythonLspVocabEntry *entry = VocabForImport(analyzer.bindings()[static_cast<size_t>(index)]);
            if (entry != nullptr && entry->detail[0] == '(') {
                label = callee + entry->detail;
                doc = entry->doc;
            }
        }
        if (label.empty()) {
            for (const PythonLspVocabEntry &e : PythonLspBuiltinVocab()) {
                if (callee != e.name || e.detail[0] != '(') continue;
                label = callee + e.detail;
                doc = e.doc;
            }
        }
        if (label.empty()) {
            for (const PythonLspVocabEntry &e : PythonLspExceptionVocab()) {
                if (callee != e.name) continue;
                label = callee + "(*args)";
                doc = e.doc;
            }
        }
    } else {
        // A dotted callee: a module function, or a method on something
        // whose type this file makes obvious.
        const int index = analyzer.ResolveFrom(scope, head);
        std::string type;
        if (index >= 0) {
            const Binding &b = analyzer.bindings()[static_cast<size_t>(index)];
            type = b.type_name.empty() ? (b.kind == BindKind::Class ? b.name : std::string()) : b.type_name;
        }
        if (type.empty() && PythonLspModuleMembers(head) != nullptr) type = "module:" + head;
        const std::string dotted_owner = callee.substr(0, callee.rfind('.'));
        if (type.rfind("module:", 0) == 0) {
            std::string module = type.substr(7);
            // `os.path.join`: walk the remaining dots through the module
            // table before giving up.
            size_t at = dotted_owner.find('.');
            while (at != std::string::npos) {
                const size_t next = dotted_owner.find('.', at + 1);
                const std::string part = dotted_owner.substr(at + 1, next == std::string::npos
                                                                         ? std::string::npos
                                                                         : next - at - 1);
                std::string candidate = module;
                candidate += ".";
                candidate += part;
                if (PythonLspModuleMembers(candidate) == nullptr) break;
                module = std::move(candidate);
                at = next;
            }
            const std::vector<PythonLspVocabEntry> *members = PythonLspModuleMembers(module);
            if (members != nullptr) {
                for (const PythonLspVocabEntry &e : *members) {
                    if (tail != e.name || e.detail[0] != '(') continue;
                    label = module;
                    label += ".";
                    label += tail;
                    label += e.detail;
                    doc = e.doc;
                }
            }
        } else if (!type.empty()) {
            const ClassInfo *cls = analyzer.ClassNamed(type);
            if (cls != nullptr) {
                std::vector<ClassMember> members;
                CollectInheritedMembers(analyzer, cls, &members);
                for (const ClassMember &m : members) {
                    if (m.name != tail || m.kind != 6) continue;
                    label = type;
                    label += ".";
                    label += tail;
                    label += m.detail;
                    doc = DocSummary(m.doc, 400);
                    drop_self = true;
                }
            } else {
                const std::vector<PythonLspVocabEntry> *members = PythonLspTypeMembers(type);
                if (members != nullptr) {
                    for (const PythonLspVocabEntry &e : *members) {
                        if (tail != e.name || e.detail[0] != '(') continue;
                        label = type;
                        label += ".";
                        label += tail;
                        label += e.detail;
                        doc = e.doc;
                    }
                }
            }
        }
    }
    if (label.empty()) return out;

    out.found = true;
    out.label = label;
    out.documentation = doc;
    out.params = SplitParams(label);
    if (drop_self && !out.params.empty() && (out.params[0] == "self" || out.params[0] == "cls")) {
        // A method called through an instance has already been given its
        // first argument, so the cursor's Nth comma is the (N+1)th
        // parameter of the definition.
        out.params.erase(out.params.begin());
    }
    out.active_param = site.active_param;
    if (!site.keyword.empty()) {
        for (size_t i = 0; i < out.params.size(); i++) {
            const std::string &p = out.params[i];
            if (p.compare(0, site.keyword.size(), site.keyword) == 0 &&
                (p.size() == site.keyword.size() || p[site.keyword.size()] == '=' ||
                 p[site.keyword.size()] == ':')) {
                out.active_param = static_cast<int>(i);
            }
        }
    }
    if (out.active_param >= static_cast<int>(out.params.size())) {
        // A `*args`/`**kwargs` tail keeps taking arguments; otherwise the
        // extra ones belong to nothing and the highlight stays on the last.
        out.active_param = out.params.empty() ? 0 : static_cast<int>(out.params.size()) - 1;
    }
    return out;
}
