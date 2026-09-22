// The analysis half of mep's own C language server. See c_lsp.h for the
// interface and for what this server does and does not claim to know;
// c_ast.h for the tree everything here answers from, and for why the
// preprocessor is modelled rather than run.
//
// Every entry point re-parses. That is not an oversight: a parse of a
// 10,000-line file is a couple of milliseconds, the server is
// single-threaded and synchronous (c_lsp_server.cpp says why), and a
// cache keyed on document text is a correctness problem waiting for a
// race rather than a speed-up anyone would notice.

#include "c_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "c_ast.h"
#include "c_lsp_std_names.h"

namespace {

// --- Small helpers ----------------------------------------------------

/** @brief Reports whether a byte can be part of a C identifier. */
bool IsWordChar(char c) { return IsCIdentChar(static_cast<unsigned char>(c)); }

/** @brief Returns a line of the document, or an empty string when the index is out of range. */
const std::string &LineAt(const std::vector<std::string> &lines, int line) {
    static const std::string kEmpty;
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return kEmpty;
    return lines[static_cast<size_t>(line)];
}

/** @brief Trims ASCII whitespace from both ends. */
std::string Trim(const std::string &s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) b--;
    return s.substr(a, b - a);
}

/** @brief Reports whether a name is declared by one of the standard headers this server knows. */
bool IsStandardName(const std::string &name) {
    size_t lo = 0;
    size_t hi = kCStdNameCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int cmp = std::strcmp(kCStdNames[mid], name.c_str());
        if (cmp == 0) return true;
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

// --- Symbols ----------------------------------------------------------

enum class SymKind {
    Function,
    Variable,
    Parameter,
    Typedef,
    Tag,  // a struct/union/enum tag, stored under "struct name"
    EnumConstant,
    Macro,
    Label,
};

struct Symbol {
    std::string name;
    SymKind kind = SymKind::Variable;
    CType type;
    CPos pos;               // where the name itself is
    int name_end_col = 0;
    unsigned flags = 0;
    bool is_definition = false;  // a function with a body, an object with storage
    bool used = false;
    bool reported = false;
    int scope = 0;               // 0 = file scope
    std::string detail;          // what hover and the symbol list show
    const CNode *node = nullptr;
    std::string path;            // "" for this document, otherwise the header it came from
};

// --- The analyzer -----------------------------------------------------

// One analysis run over one document. Holds the parse, the scope stack
// and everything harvested from the headers this document includes; each
// public entry point in c_lsp.h builds one, runs it, and throws it away.
class Analyzer {
public:
    Analyzer(const std::vector<std::string> &lines, const CLspOptions &opts)
        : lines_(lines), opts_(opts), parse_(ParseC(lines)) {}

    const CParseResult &parse() const { return parse_; }
    const std::vector<Symbol> &symbols() const { return symbols_; }
    const std::vector<CLspDiagnostic> &diagnostics() const { return diags_; }

    /** @brief Runs the collection passes: includes, file-scope declarations, then every function body. */
    void Collect() {
        CollectMacros();
        CollectMentionedElsewhere();
        if (opts_.check_files) CollectIncludes();
        else NoteSystemIncludes();
        PushScope();
        CollectFileScope(parse_.unit.get());
        collected_ = true;
    }

    /** @brief Runs every check, filling the diagnostic list. */
    void Check() {
        if (!collected_) Collect();
        ReportParseErrors();
        ReportPreprocessorProblems();
        WalkFileScope(parse_.unit.get());
        FlushUndeclared();
        ReportUnusedFileScope();
        ReportLineLengths();
        std::stable_sort(diags_.begin(), diags_.end(), [](const CLspDiagnostic &a, const CLspDiagnostic &b) {
            return a.line != b.line ? a.line < b.line : a.col_start < b.col_start;
        });
    }

    // --- Lookups the feature functions need ---------------------------

    /** @brief Finds a file-scope or header symbol by name, or nullptr. */
    const Symbol *FindGlobal(const std::string &name) const {
        const auto it = globals_.find(name);
        return it == globals_.end() ? nullptr : &symbols_[it->second];
    }

    /** @brief Finds a struct/union/enum definition by its tag spelling ("struct node"), or nullptr. */
    const CNode *FindTag(const std::string &tag) const {
        const auto it = tags_.find(tag);
        return it == tags_.end() ? nullptr : it->second;
    }

    /** @brief Finds a macro definition by name, or nullptr. */
    const CDirective *FindMacro(const std::string &name) const {
        const auto it = macros_.find(name);
        return it == macros_.end() ? nullptr : it->second;
    }

    /** @brief Resolves a typedef name to the type it stands for, following chains. */
    CType ResolveTypedef(const CType &type) const {
        CType t = type;
        for (int hops = 0; hops < 8; hops++) {
            const auto it = typedefs_.find(t.base);
            if (it == typedefs_.end()) break;
            CType next = it->second;
            next.pointers += t.pointers;
            next.array_dims += t.array_dims;
            if (next.base == t.base) break;
            t = next;
        }
        return t;
    }

    /** @brief Resolves a type to the struct or union declaration behind it, or nullptr. */
    const CNode *RecordOf(const CType &type) const {
        const CType t = ResolveTypedef(type);
        if (!t.is_struct && t.base.rfind("struct ", 0) != 0 && t.base.rfind("union ", 0) != 0) return nullptr;
        return FindTag(t.base);
    }

    /** @brief Whether this document `#include`s a header this server has no declarations for. */
    bool has_opaque_include() const { return opaque_include_; }

    const std::map<std::string, std::string> &resolved_includes() const { return resolved_includes_; }
    const std::unordered_map<std::string, CType> &typedefs() const { return typedefs_; }
    const std::unordered_map<std::string, const CNode *> &tags() const { return tags_; }
    const std::unordered_map<std::string, const CDirective *> &macros() const { return macros_; }

private:
    const std::vector<std::string> &lines_;
    CLspOptions opts_;
    CParseResult parse_;
    std::vector<CLspDiagnostic> diags_;
    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, size_t> globals_;
    std::unordered_map<std::string, const CNode *> tags_;
    std::unordered_map<std::string, const CDirective *> macros_;
    std::unordered_map<std::string, CType> typedefs_;
    std::map<std::string, std::string> resolved_includes_;  // header spelling -> absolute path
    // Names that appear somewhere this analysis deliberately does not
    // read: inside a `#if` arm that was not taken, or inside a macro
    // body. A name mentioned there is never reported as unused, because
    // the mention this server cannot see is exactly the one that makes
    // it used.
    std::unordered_set<std::string> mentioned_elsewhere_;
    bool opaque_include_ = false;
    bool collected_ = false;
    bool in_header_ = false;

    // Scope stack. Index 0 is file scope; a function body, and every
    // block inside it, pushes another.
    std::vector<std::unordered_map<std::string, size_t>> scopes_;
    // Per-function state, reset by WalkFunction.
    const CNode *current_function_ = nullptr;
    std::vector<Symbol> labels_;
    std::vector<std::pair<std::string, CPos>> gotos_;

    // --- Diagnostics --------------------------------------------------

    /** @brief Records one diagnostic over a single line's column span. */
    void Report(int line, int col_start, int col_end, CLspSeverity severity, const char *code,
                const std::string &message) {
        CLspDiagnostic d;
        d.line = line;
        d.col_start = col_start;
        d.col_end = col_end > col_start ? col_end : col_start + 1;
        d.severity = severity;
        d.code = code;
        d.message = message;
        diags_.push_back(d);
    }

    /** @brief Records one diagnostic over a node's name. */
    void ReportName(const CNode *node, CLspSeverity severity, const char *code, const std::string &message) {
        Report(node->name_pos.line, node->name_pos.col, node->name_end_col, severity, code, message);
    }

    /** @brief Copies the parser's syntax errors into the diagnostic list. */
    void ReportParseErrors() {
        for (const CSyntaxError &e : parse_.errors) {
            const int end = e.end.line == e.start.line ? e.end.col : e.start.col + 1;
            Report(e.start.line, e.start.col, end, CLspSeverity::Error, e.code.c_str(), e.message);
        }
    }

    /** @brief Reports whether the file has a syntax error, which several checks refuse to run past. */
    bool HasSyntaxError() const { return !parse_.errors.empty(); }

    // --- Collection ---------------------------------------------------

    /** @brief Pushes a new scope onto the stack. */
    void PushScope() { scopes_.emplace_back(); }

    /**
     * @brief Pops the innermost scope, reporting the locals nothing in it read.
     */
    void PopScope() {
        for (const auto &entry : scopes_.back()) {
            Symbol &sym = symbols_[entry.second];
            if (sym.used || sym.reported) continue;
            if (sym.kind != SymKind::Variable) continue;
            if ((sym.flags & (kCFlagExtern | kCFlagAttrUnused)) != 0) continue;
            if (mentioned_elsewhere_.count(sym.name) > 0) continue;
            sym.reported = true;
            Report(sym.pos.line, sym.pos.col, sym.name_end_col, CLspSeverity::Warning, "unused-variable",
                   "`" + sym.name + "` is set up here and never read");
        }
        scopes_.pop_back();
    }

    /** @brief Declares a symbol in the innermost scope, returning its index. */
    size_t Declare(Symbol sym) {
        sym.scope = static_cast<int>(scopes_.size()) - 1;
        symbols_.push_back(std::move(sym));
        const size_t index = symbols_.size() - 1;
        if (!scopes_.empty()) scopes_.back()[symbols_[index].name] = index;
        if (scopes_.size() == 1) globals_[symbols_[index].name] = index;
        return index;
    }

    /** @brief Finds a symbol by name from the innermost scope outwards, or nullptr. */
    Symbol *Lookup(const std::string &name) {
        for (size_t i = scopes_.size(); i > 0; i--) {
            const auto it = scopes_[i - 1].find(name);
            if (it != scopes_[i - 1].end()) return &symbols_[it->second];
        }
        // A name harvested from an included header lives in the global
        // table without being in any scope, because the headers are read
        // before the file's own scope stack exists.
        const auto global = globals_.find(name);
        return global == globals_.end() ? nullptr : &symbols_[global->second];
    }

    /** @brief Records every `#define` this file makes, in the arms that were taken. */
    void CollectMacros() {
        for (const CDirective &d : parse_.directives) {
            if (d.kind != CDirectiveKind::Define || d.name.empty()) continue;
            macros_[d.name] = &d;
        }
    }

    /**
     * @brief Collects the names that appear only where this analysis cannot follow them.
     *
     * A `static` helper called once, from inside a `#if` arm this server
     * did not take, is not dead code -- it is code this server cannot
     * see. Same for a local whose only use is inside a macro body. The
     * cheapest correct answer is to notice the name is *written* there
     * and stop asking about it.
     */
    void CollectMentionedElsewhere() {
        for (const CToken &t : parse_.tokens) {
            if (t.kind != CTokKind::Ident) continue;
            if (t.active && t.directive < 0) continue;
            mentioned_elsewhere_.insert(t.text);
        }
        // Macro bodies are text, not tokens, so scan them as text.
        for (const CDirective &d : parse_.directives) {
            if (d.kind != CDirectiveKind::Define) continue;
            size_t i = 0;
            while (i < d.body.size()) {
                if (!IsWordChar(d.body[i])) {
                    i++;
                    continue;
                }
                const size_t from = i;
                while (i < d.body.size() && IsWordChar(d.body[i])) i++;
                mentioned_elsewhere_.insert(d.body.substr(from, i - from));
            }
        }
    }

    /** @brief Notes whether any `#include` is of a header this server has no table for. */
    void NoteSystemIncludes() {
        for (const CDirective &d : parse_.directives) {
            if (d.kind != CDirectiveKind::Include || !d.active) continue;
            if (d.header.empty()) {
                opaque_include_ = true;  // a macro-expanded include target
                continue;
            }
            if (d.angled && CLspKnowsHeader(d.header)) continue;
            opaque_include_ = true;
        }
    }

    /** @brief Resolves `#include "..."` against the document's directory and harvests what each header declares. */
    void CollectIncludes() {
        if (opts_.doc_dir.empty()) {
            NoteSystemIncludes();
            return;
        }
        std::vector<std::string> queue;
        for (const CDirective &d : parse_.directives) {
            if (d.kind != CDirectiveKind::Include || d.header.empty() || !d.active) continue;
            const std::string path = ResolveInclude(d.header);
            if (path.empty()) continue;
            resolved_includes_[d.header] = path;
            queue.push_back(path);
        }
        // Which of this document's own includes are out of reach, now
        // that the resolutions are known. HarvestHeader adds to this as
        // it goes, for the headers *those* headers include.
        opaque_include_ = false;
        for (const CDirective &d : parse_.directives) {
            if (d.kind != CDirectiveKind::Include || !d.active) continue;
            if (d.header.empty()) {
                opaque_include_ = true;  // a macro-expanded include target
                continue;
            }
            if (resolved_includes_.count(d.header) > 0) continue;
            if (d.angled && CLspKnowsHeader(d.header)) continue;
            opaque_include_ = true;
        }

        // Two levels deep and twenty files at most: the first level is
        // what the document includes, the second what those include.
        // Following the whole graph would turn a keystroke into a
        // filesystem walk.
        std::set<std::string> seen(queue.begin(), queue.end());
        const size_t first_level = queue.size();
        size_t at = 0;
        int budget = 20;
        while (at < queue.size() && budget > 0) {
            const bool follow = at < first_level;
            const std::string path = queue[at++];
            budget--;
            HarvestHeader(path, follow ? &queue : nullptr, &seen);
        }
    }

    /**
     * @brief Finds the file a quoted include names, searching the places a project keeps its headers.
     *
     * The directory the file is in, then `include/` and `src/` beside
     * it, then the same three one level up, and so on to the repository
     * root. That is where a project's own headers are; what it is *not*
     * is the `-I` list a build system would hand a compiler, which this
     * server has no way to know. IncludeSearchRoots is therefore a good
     * guess and never a complete one, which is why a failure to resolve
     * is only ever a hint (see ReportPreprocessorProblems).
     */
    std::string ResolveInclude(const std::string &header) const {
        if (opts_.doc_dir.empty()) return std::string();
        namespace fs = std::filesystem;
        for (const std::string &root : IncludeSearchRoots()) {
            std::error_code ec;
            const fs::path candidate = fs::path(root) / header;
            if (fs::is_regular_file(candidate, ec)) {
                const fs::path canon = fs::weakly_canonical(candidate, ec);
                return ec ? candidate.string() : canon.string();
            }
        }
        return std::string();
    }

    /** @brief The directories a quoted include is looked for in, nearest first, built once per analysis. */
    const std::vector<std::string> &IncludeSearchRoots() const {
        if (!search_roots_.empty()) return search_roots_;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir(opts_.doc_dir);
        for (int up = 0; up < 5 && !dir.empty(); up++) {
            search_roots_.push_back(dir.string());
            search_roots_.push_back((dir / "include").string());
            search_roots_.push_back((dir / "src").string());
            // A repository root is as far up as a header can sensibly be.
            if (fs::exists(dir / ".git", ec)) break;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return search_roots_;
    }

    mutable std::vector<std::string> search_roots_;

    /** @brief Reads a header and records the declarations, tags, typedefs and macros it introduces. */
    void HarvestHeader(const std::string &path, std::vector<std::string> *queue, std::set<std::string> *seen) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        if (text.size() > 1u << 20) return;  // a megabyte of header is not a project's own
        std::vector<std::string> header_lines;
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                header_lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        header_lines.push_back(cur);
        header_parses_.push_back(std::make_unique<CParseResult>(ParseC(header_lines)));
        const CParseResult &hp = *header_parses_.back();
        for (const CDirective &d : hp.directives) {
            if (d.kind == CDirectiveKind::Define && !d.name.empty() && macros_.count(d.name) == 0) {
                macros_[d.name] = &d;
            }
            if (d.kind != CDirectiveKind::Include) continue;
            // A header this one includes and this server cannot read is
            // just as opaque as one the document includes directly: the
            // declarations behind it are equally out of reach, and the
            // undeclared-name check has to stay quiet about them.
            if (d.header.empty() || (d.angled && !CLspKnowsHeader(d.header))) {
                opaque_include_ = true;
                continue;
            }
            if (d.angled) continue;
            const std::string nested = ResolveInclude(d.header);
            if (nested.empty()) {
                opaque_include_ = true;
                continue;
            }
            if (queue != nullptr && seen->insert(nested).second) queue->push_back(nested);
        }
        HarvestDeclarations(hp.unit.get(), path);
    }

    std::vector<std::unique_ptr<CParseResult>> header_parses_;

    /** @brief Records a header's top-level declarations as symbols of that file. */
    void HarvestDeclarations(const CNode *unit, const std::string &path) {
        for (const CNodePtr &node : unit->body) {
            switch (node->kind) {
                case CNodeKind::FunctionDef:
                    AddHeaderSymbol(node.get(), SymKind::Function, node->name, path);
                    break;
                case CNodeKind::Declaration:
                    RegisterTags(node.get(), path);
                    for (const CNodePtr &d : node->kids) {
                        if (d->kind != CNodeKind::Declarator || d->name.empty()) continue;
                        if ((node->flags & kCFlagTypedef) != 0) {
                            typedefs_.emplace(d->name, d->type);
                            AddHeaderSymbol(d.get(), SymKind::Typedef, d->name, path);
                        } else {
                            AddHeaderSymbol(d.get(), d->type.is_function ? SymKind::Function : SymKind::Variable,
                                            d->name, path);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }

    /** @brief Adds one symbol harvested from a header, without overriding this document's own. */
    void AddHeaderSymbol(const CNode *node, SymKind kind, const std::string &name, const std::string &path) {
        if (name.empty() || globals_.count(name) > 0) return;
        Symbol sym;
        sym.name = name;
        sym.kind = kind;
        sym.type = node->type;
        sym.pos = node->name_pos;
        sym.name_end_col = node->name_end_col;
        sym.flags = node->flags;
        sym.node = node;
        sym.path = path;
        sym.used = true;  // nothing in a header is this document's to call dead
        sym.detail = DescribeDeclaration(node);
        symbols_.push_back(std::move(sym));
        globals_[name] = symbols_.size() - 1;
    }

    /** @brief Records every struct, union and enum a declaration defines, and their enumeration constants. */
    void RegisterTags(const CNode *decl, const std::string &path) {
        for (const CNodePtr &kid : decl->kids) {
            if (kid->kind == CNodeKind::RecordDecl) {
                if (!kid->name.empty() && !kid->body.empty()) {
                    tags_.emplace(kid->str_value + " " + kid->name, kid.get());
                }
                for (const CNodePtr &field : kid->body) RegisterNestedTags(field.get(), path);
            } else if (kid->kind == CNodeKind::EnumDecl) {
                if (!kid->name.empty()) tags_.emplace("enum " + kid->name, kid.get());
                for (const CNodePtr &e : kid->body) {
                    if (e->kind != CNodeKind::Enumerator || e->name.empty()) continue;
                    if (globals_.count(e->name) > 0) continue;
                    Symbol sym;
                    sym.name = e->name;
                    sym.kind = SymKind::EnumConstant;
                    sym.pos = e->name_pos;
                    sym.name_end_col = e->name_end_col;
                    sym.node = e.get();
                    sym.path = path;
                    sym.used = !path.empty();
                    sym.detail = "enumeration constant of " +
                                 (kid->name.empty() ? std::string("an anonymous enum") : "enum " + kid->name);
                    if (scopes_.empty()) {
                        symbols_.push_back(std::move(sym));
                        globals_[e->name] = symbols_.size() - 1;
                    } else {
                        Declare(std::move(sym));
                    }
                }
            }
        }
    }

    /** @brief Registers the tags a struct member's own inline definition introduces. */
    void RegisterNestedTags(const CNode *field, const std::string &path) {
        for (const CNodePtr &kid : field->kids) {
            if (kid->kind == CNodeKind::RecordDecl && !kid->name.empty() && !kid->body.empty()) {
                tags_.emplace(kid->str_value + " " + kid->name, kid.get());
            }
        }
        (void)path;
    }

    /** @brief Collects this document's own file-scope declarations before any body is walked. */
    void CollectFileScope(const CNode *unit) {
        in_header_ = opts_.file_name.size() > 2 &&
                     opts_.file_name.compare(opts_.file_name.size() - 2, 2, ".h") == 0;
        for (const CNodePtr &node : unit->body) {
            switch (node->kind) {
                case CNodeKind::FunctionDef:
                    DeclareOwn(node.get(), SymKind::Function, node->name, node->flags, true);
                    break;
                case CNodeKind::Declaration: {
                    RegisterTags(node.get(), std::string());
                    for (const CNodePtr &d : node->kids) {
                        if (d->kind != CNodeKind::Declarator || d->name.empty()) continue;
                        if ((node->flags & kCFlagTypedef) != 0) {
                            typedefs_[d->name] = d->type;
                            DeclareOwn(d.get(), SymKind::Typedef, d->name, node->flags, true);
                        } else {
                            // `static int x;` twice is two tentative
                            // definitions, which C explicitly allows;
                            // only an initializer makes one *the*
                            // definition.
                            const bool defines =
                                (node->flags & kCFlagExtern) == 0 && !d->type.is_function && !d->kids.empty();
                            DeclareOwn(d.get(), d->type.is_function ? SymKind::Function : SymKind::Variable,
                                       d->name, node->flags | d->flags, defines);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    /** @brief Declares one of this document's own file-scope symbols, reporting a conflicting redeclaration. */
    void DeclareOwn(const CNode *node, SymKind kind, const std::string &name, unsigned flags, bool is_definition) {
        if (name.empty()) return;
        const auto it = globals_.find(name);
        if (it != globals_.end() && symbols_[it->second].path.empty()) {
            Symbol &prev = symbols_[it->second];
            // Two definitions of the same thing is an error a compiler
            // would refuse; a declaration and its definition is the
            // normal shape of a C file and must stay silent.
            if (prev.is_definition && is_definition && kind != SymKind::Typedef) {
                Report(node->name_pos.line, node->name_pos.col, node->name_end_col, CLspSeverity::Error,
                       "redefinition",
                       "`" + name + "` is already defined on line " + std::to_string(prev.pos.line + 1));
            }
            if (is_definition) {
                prev.is_definition = true;
                prev.pos = node->name_pos;
                prev.name_end_col = node->name_end_col;
                prev.node = node;
                prev.detail = DescribeDeclaration(node);
                prev.flags |= flags;
            }
            return;
        }
        Symbol sym;
        sym.name = name;
        sym.kind = kind;
        sym.type = node->type;
        sym.pos = node->name_pos;
        sym.name_end_col = node->name_end_col;
        sym.flags = flags;
        sym.is_definition = is_definition;
        sym.node = node;
        sym.detail = DescribeDeclaration(node);
        Declare(std::move(sym));
    }

    /** @brief Renders a declaration the way hover and the symbol list want to read it. */
    static std::string DescribeDeclaration(const CNode *node) {
        std::string prefix;
        if ((node->flags & kCFlagTypedef) != 0) prefix = "typedef ";
        else if ((node->flags & kCFlagStatic) != 0) prefix = "static ";
        else if ((node->flags & kCFlagExtern) != 0) prefix = "extern ";
        std::string out = prefix + (node->type.spelling.empty() ? std::string("int") : node->type.spelling);
        if (!out.empty() && out.back() != '*' && out.back() != ' ') out += ' ';
        out += node->name;
        if (node->type.is_function || !node->params.empty()) {
            out += "(";
            for (size_t i = 0; i < node->params.size(); i++) {
                if (i > 0) out += ", ";
                out += DescribeParameter(node->params[i].get());
            }
            if (node->params.empty()) out += "void";
            out += ")";
        }
        for (int i = 0; i < node->type.array_dims; i++) out += "[]";
        return out;
    }

    /** @brief Renders one parameter for a signature line. */
    static std::string DescribeParameter(const CNode *param) {
        if (param->name == "...") return "...";
        std::string out = param->type.spelling.empty() ? std::string("int") : param->type.spelling;
        if (!param->name.empty()) {
            if (!out.empty() && out.back() != '*') out += ' ';
            out += param->name;
        }
        return out;
    }

    // --- Preprocessor checks ------------------------------------------

    /** @brief Reports the preprocessor problems a reader can confirm from this file alone. */
    void ReportPreprocessorProblems() {
        std::unordered_map<std::string, const CDirective *> seen;
        for (const CDirective &d : parse_.directives) {
            if (d.kind == CDirectiveKind::Undef && !d.name.empty()) seen.erase(d.name);
            if (d.kind == CDirectiveKind::Define && !d.name.empty()) {
                const auto it = seen.find(d.name);
                // A redefinition with the same replacement text is legal
                // and common (two headers guarding the same constant);
                // one with a different body is the mistake worth naming.
                // An `#undef` in between makes it deliberate, so the
                // loop above drops the record when it sees one.
                if (it != seen.end() && it->second->body != d.body && d.active && it->second->active) {
                    Report(d.name_pos.line, d.name_pos.col, d.name_end_col, CLspSeverity::Warning,
                           "macro-redefined",
                           "`" + d.name + "` was already defined differently on line " +
                               std::to_string(it->second->line + 1));
                }
                seen[d.name] = &d;
            }
            if (d.kind != CDirectiveKind::Include || !d.active || d.header.empty()) continue;
            if (!opts_.check_files || opts_.doc_dir.empty() || d.angled) continue;
            if (resolved_includes_.count(d.header) > 0) continue;
            // Only worth saying when the search path is evidently right
            // for this project: if *no* quoted include in the file
            // resolved, the headers are somewhere a build system knows
            // about and this server does not, and six reports saying so
            // help nobody. One failure among several successes is the
            // shape a typo has.
            if (resolved_includes_.empty()) continue;
            // A hint, not a warning: the header may well exist
            // somewhere on a `-I` path this server was never told about,
            // and an editor that shouts "no such file" at every
            // correctly-built project is one people turn off.
            Report(d.line, d.header_col, d.header_end_col, CLspSeverity::Hint, "include-not-found",
                   "No file named `" + d.header + "` in this file's directory or its project's `include`");
        }
    }

    // --- The walk -----------------------------------------------------

    /** @brief Walks every top-level construct, checking what it contains. */
    void WalkFileScope(const CNode *unit) {
        for (const CNodePtr &node : unit->body) {
            if (node->kind == CNodeKind::FunctionDef) {
                WalkFunction(node.get());
                continue;
            }
            if (node->kind == CNodeKind::MacroDecl) {
                // `WRAPPER(int, open)(const char *path, ...) { ... }`:
                // the parameters are inside the macro arguments, so
                // nothing in this body can be placed and nothing in it
                // may be reported as unplaceable.
                for (const CNodePtr &arg : node->kids) {
                    if (arg->kind != CNodeKind::Ident) continue;
                    if (Symbol *sym = Lookup(arg->name); sym != nullptr) sym->used = true;
                }
                suppress_undeclared_++;
                PushScope();
                for (const CNodePtr &stmt : node->body) WalkStatement(stmt.get());
                PopScope();
                suppress_undeclared_--;
                continue;
            }
            if (node->kind == CNodeKind::Declaration) {
                CheckRecordDefinitions(node.get());
                for (const CNodePtr &d : node->kids) {
                    if (d->kind != CNodeKind::Declarator) continue;
                    for (const CNodePtr &init : d->kids) WalkExpression(init.get());
                }
                continue;
            }
            if (node->kind == CNodeKind::StaticAssert) {
                for (const CNodePtr &kid : node->kids) WalkExpression(kid.get());
            }
        }
    }

    /** @brief Checks one function definition: its parameters, its body, and the shape of its returns. */
    void WalkFunction(const CNode *fn) {
        const CNode *saved = current_function_;
        current_function_ = fn;
        labels_.clear();
        gotos_.clear();
        PushScope();
        std::unordered_set<std::string> param_names;
        for (const CNodePtr &param : fn->params) {
            if (param->name.empty() || param->name == "...") continue;
            if (!param_names.insert(param->name).second) {
                ReportName(param.get(), CLspSeverity::Error, "duplicate-parameter",
                           "`" + param->name + "` is already a parameter of this function");
            }
            Symbol sym;
            sym.name = param->name;
            sym.kind = SymKind::Parameter;
            sym.type = param->type;
            sym.pos = param->name_pos;
            sym.name_end_col = param->name_end_col;
            sym.flags = param->flags;
            sym.node = param.get();
            sym.used = true;  // an unused parameter is an interface, not a mistake
            sym.detail = DescribeParameter(param.get());
            Declare(std::move(sym));
        }
        CollectLabels(fn->body);
        WalkBlock(fn->body, false);
        CheckReturns(fn);
        CheckLabels();
        PopScope();
        current_function_ = saved;
    }

    /** @brief Walks a list of statements in a new scope, reporting anything after a jump. */
    void WalkBlock(const std::vector<CNodePtr> &body, bool push) {
        if (push) PushScope();
        const CNode *jump = nullptr;
        bool reported = false;
        for (const CNodePtr &stmt : body) {
            if (stmt == nullptr) continue;
            // A label, a `case` or a `default` is reachable from
            // somewhere else by definition, so the block is live again
            // from there on. Getting this wrong makes every statement
            // after the first `break;` of a `switch` a false report.
            if (IsLabelLike(stmt.get())) jump = nullptr;
            if (jump != nullptr && !reported && IsReachableStart(stmt.get())) {
                Report(stmt->start.line, stmt->start.col, stmt->start.col + 1, CLspSeverity::Warning,
                       "unreachable-code", "Nothing can reach this: the block already returned or jumped");
                reported = true;  // one report per block is enough to make the point
            }
            WalkStatement(stmt.get());
            if (IsJump(stmt.get())) jump = stmt.get();
        }
        if (push) PopScope();
    }

    /** @brief Reports whether a statement always leaves the block it is in. */
    static bool IsJump(const CNode *stmt) {
        switch (stmt->kind) {
            case CNodeKind::ReturnStmt:
            case CNodeKind::BreakStmt:
            case CNodeKind::ContinueStmt:
            case CNodeKind::GotoStmt:
                return true;
            default:
                return false;
        }
    }

    /** @brief Reports whether a statement carries a label, making the code from there on reachable again. */
    static bool IsLabelLike(const CNode *stmt) {
        return stmt->kind == CNodeKind::LabelStmt || stmt->kind == CNodeKind::CaseStmt ||
               stmt->kind == CNodeKind::DefaultStmt;
    }

    /** @brief Reports whether a statement is one that code after a jump could still legitimately be. */
    static bool IsReachableStart(const CNode *stmt) {
        // A label, a `case` or a declaration after a `return` is normal
        // C: the label is jumped to, and a declaration without an
        // initializer does nothing at all.
        switch (stmt->kind) {
            case CNodeKind::LabelStmt:
            case CNodeKind::CaseStmt:
            case CNodeKind::DefaultStmt:
            case CNodeKind::DeclStmt:
            case CNodeKind::EmptyStmt:
                return false;
            default:
                return true;
        }
    }

    /** @brief Walks one statement, checking it and everything inside it. */
    void WalkStatement(const CNode *stmt) {
        if (stmt == nullptr) return;
        switch (stmt->kind) {
            case CNodeKind::CompoundStmt:
                WalkBlock(stmt->body, true);
                return;
            case CNodeKind::DeclStmt:
                WalkDeclaration(stmt->kids.empty() ? nullptr : stmt->kids.front().get());
                return;
            case CNodeKind::IfStmt:
                CheckCondition(stmt->kids.front().get(), "if");
                WalkExpression(stmt->kids.front().get());
                CheckEmptyBody(stmt);
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                if (!stmt->orelse.empty()) WalkStatement(stmt->orelse.front().get());
                return;
            case CNodeKind::WhileStmt:
                CheckCondition(stmt->kids.front().get(), "while");
                WalkExpression(stmt->kids.front().get());
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                return;
            case CNodeKind::DoStmt:
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                if (!stmt->kids.empty()) {
                    CheckCondition(stmt->kids.front().get(), "while");
                    WalkExpression(stmt->kids.front().get());
                }
                return;
            case CNodeKind::ForStmt: {
                PushScope();
                if (!stmt->kids.empty() && stmt->kids[0]->kind == CNodeKind::DeclStmt) {
                    WalkDeclaration(stmt->kids[0]->kids.empty() ? nullptr : stmt->kids[0]->kids.front().get());
                } else if (!stmt->kids.empty()) {
                    WalkStatement(stmt->kids[0].get());
                }
                for (size_t i = 1; i < stmt->kids.size(); i++) WalkExpression(stmt->kids[i].get());
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                PopScope();
                return;
            }
            case CNodeKind::SwitchStmt:
                WalkExpression(stmt->kids.empty() ? nullptr : stmt->kids.front().get());
                CheckSwitch(stmt);
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                return;
            case CNodeKind::CaseStmt:
            case CNodeKind::DefaultStmt:
                for (const CNodePtr &kid : stmt->kids) WalkExpression(kid.get());
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                return;
            case CNodeKind::LabelStmt:
                WalkStatement(stmt->body.empty() ? nullptr : stmt->body.front().get());
                return;
            case CNodeKind::GotoStmt:
                if (!stmt->name.empty()) gotos_.emplace_back(stmt->name, stmt->name_pos);
                for (const CNodePtr &kid : stmt->kids) WalkExpression(kid.get());
                return;
            case CNodeKind::ExprStmt:
                if (!stmt->kids.empty()) {
                    CheckStatementHasEffect(stmt->kids.front().get());
                    WalkExpression(stmt->kids.front().get());
                }
                for (const CNodePtr &block : stmt->body) WalkStatement(block.get());
                return;
            case CNodeKind::ReturnStmt:
            case CNodeKind::StaticAssert:
                for (const CNodePtr &kid : stmt->kids) WalkExpression(kid.get());
                return;
            default:
                for (const CNodePtr &kid : stmt->kids) WalkExpression(kid.get());
                for (const CNodePtr &kid : stmt->body) WalkStatement(kid.get());
                return;
        }
    }

    /** @brief Declares a block-scope declaration's names and walks its initializers. */
    void WalkDeclaration(const CNode *decl) {
        if (decl == nullptr) return;
        CheckRecordDefinitions(decl);
        RegisterTags(decl, std::string());
        for (const CNodePtr &d : decl->kids) {
            if (d->kind != CNodeKind::Declarator || d->name.empty()) continue;
            const SymKind kind = (decl->flags & kCFlagTypedef) != 0 ? SymKind::Typedef
                                 : d->type.is_function              ? SymKind::Function
                                                                    : SymKind::Variable;
            if (kind == SymKind::Typedef) typedefs_[d->name] = d->type;
            // A shadowed name is worth a hint, but only for the ones
            // that mean something different in the inner scope: a
            // parameter named after a global is how C is written.
            if (Symbol *outer = Lookup(d->name); outer != nullptr && outer->scope > 0 &&
                                                 outer->kind == SymKind::Variable && kind == SymKind::Variable) {
                Report(d->name_pos.line, d->name_pos.col, d->name_end_col, CLspSeverity::Hint, "shadowed-variable",
                       "`" + d->name + "` hides the one declared on line " + std::to_string(outer->pos.line + 1));
            }
            Symbol sym;
            sym.name = d->name;
            sym.kind = kind;
            sym.type = d->type;
            sym.pos = d->name_pos;
            sym.name_end_col = d->name_end_col;
            sym.flags = decl->flags | d->flags;
            sym.is_definition = true;
            sym.node = d.get();
            sym.detail = DescribeDeclaration(d.get());
            if (kind == SymKind::Function || (sym.flags & kCFlagExtern) != 0) sym.used = true;
            Declare(std::move(sym));
            // After declaring, not before: C says a name's scope begins
            // as soon as its declarator is complete, which is what makes
            // `struct s *p = malloc(sizeof *p);` legal -- and it is how
            // everyone writes that line.
            for (const CNodePtr &init : d->kids) WalkExpression(init.get());
        }
    }

    /** @brief Walks an expression, resolving every name it reads. */
    void WalkExpression(const CNode *expr) {
        if (expr == nullptr) return;
        switch (expr->kind) {
            case CNodeKind::Ident:
                ResolveUse(expr);
                return;
            case CNodeKind::Member:
            case CNodeKind::Arrow:
                WalkExpression(expr->kids.empty() ? nullptr : expr->kids.front().get());
                CheckMember(expr);
                return;
            case CNodeKind::Call: {
                CheckCall(expr);
                // A function-like macro's arguments are text, not
                // values: `FIX(gecos)` names a struct member, and only
                // the macro's own body says so. Resolving them as
                // identifiers is how a server invents a screenful of
                // undeclared names out of one macro.
                const CDirective *macro = expr->name.empty() ? nullptr : FindMacro(expr->name);
                const bool text_arguments = macro != nullptr && macro->function_like;
                if (text_arguments) suppress_undeclared_++;
                for (const CNodePtr &kid : expr->kids) WalkExpression(kid.get());
                if (text_arguments) suppress_undeclared_--;
                return;
            }
            case CNodeKind::Assign:
                CheckSelfAssignment(expr);
                for (const CNodePtr &kid : expr->kids) WalkExpression(kid.get());
                return;
            case CNodeKind::Binary:
                CheckBitwisePrecedence(expr);
                for (const CNodePtr &kid : expr->kids) WalkExpression(kid.get());
                return;
            case CNodeKind::CharLit:
                CheckCharLiteral(expr);
                return;
            case CNodeKind::TypeName:
            case CNodeKind::Number:
            case CNodeKind::StrLit:
                return;
            case CNodeKind::StmtExpr:
                WalkBlock(expr->body, true);
                return;
            case CNodeKind::Designator:
                // `.field = value` names a member, not a variable; only
                // the value is an expression.
                if (!expr->kids.empty()) WalkExpression(expr->kids.back().get());
                return;
            default:
                for (const CNodePtr &kid : expr->kids) WalkExpression(kid.get());
                for (const CNodePtr &kid : expr->body) WalkStatement(kid.get());
                return;
        }
    }

    /**
     * @brief Resolves one identifier read, marking what it names as used or reporting that nothing does.
     *
     * The undeclared-name report is the one this server is most careful
     * about, because it is the one most able to be wrong: it is only
     * made when every declaration in play is actually in reach -- this
     * file, the headers next to it that resolved, and the standard
     * library -- and it is switched off entirely for a file that
     * includes something opaque, or that has a syntax error in it (a
     * file mid-edit is exactly when an invented "undeclared" is worst).
     */
    void ResolveUse(const CNode *node) {
        if (node->name.empty()) return;
        if (Symbol *sym = Lookup(node->name); sym != nullptr) {
            sym->used = true;
            return;
        }
        if (macros_.count(node->name) > 0) return;
        if (!ShouldReportUndeclared()) return;
        // Every compiler has its own builtins, and a file that calls one
        // is not a file with a typo in it.
        if (node->name.rfind("__builtin_", 0) == 0) return;
        // `__` and `_X` are reserved for the implementation, so a name
        // spelled that way was declared by the implementation --
        // somewhere this server was not given the headers to look.
        if (node->name.rfind("__", 0) == 0) return;
        if (node->name.size() > 1 && node->name[0] == '_' && std::isupper(static_cast<unsigned char>(node->name[1]))) {
            return;
        }
        if (IsStandardName(node->name)) return;
        if (mentioned_elsewhere_.count(node->name) > 0) return;
        CLspDiagnostic d;
        d.line = node->name_pos.line;
        d.col_start = node->name_pos.col;
        d.col_end = node->name_end_col;
        d.severity = CLspSeverity::Error;
        d.code = "undeclared-name";
        d.message = "Nothing declares `" + node->name + "` here";
        pending_undeclared_.push_back(std::move(d));
        unplaceable_names_.insert(node->name);
    }

    /** @brief Whether the undeclared-name check may speak at all for this document. */
    bool ShouldReportUndeclared() const {
        return !opaque_include_ && !HasSyntaxError() && suppress_undeclared_ == 0;
    }

    /**
     * @brief Commits or discards the undeclared-name reports, depending on how many distinct names there were.
     *
     * A handful of unplaceable names in a file is a handful of typos. Six
     * or more is a file whose declarations are somewhere this server
     * cannot reach -- a platform header it has no table for, a macro
     * that expands to a declaration, a generated body -- and in that
     * situation every single report is wrong. The threshold is what
     * makes the check survive contact with real code instead of being
     * switched off; mep's R server carries the same rule for the same
     * reason.
     */
    void FlushUndeclared() {
        if (unplaceable_names_.size() >= 6) return;
        // A name used four times and declared nowhere is not a typo --
        // nobody misspells the same identifier four times. It is a name
        // a macro brought into scope: tree-sitter's generated parsers
        // get `lookahead` from `START_LEXER()`, and reporting it forty
        // times is the single loudest thing this check can do wrong.
        std::unordered_map<std::string, int> uses;
        for (const CLspDiagnostic &d : pending_undeclared_) uses[NameIn(d.message)]++;
        for (CLspDiagnostic &d : pending_undeclared_) {
            if (uses[NameIn(d.message)] >= 4) continue;
            diags_.push_back(std::move(d));
        }
    }

    /** @brief Pulls the backquoted name back out of an undeclared-name message. */
    static std::string NameIn(const std::string &message) {
        const size_t open = message.find('`');
        if (open == std::string::npos) return message;
        const size_t close = message.find('`', open + 1);
        if (close == std::string::npos) return message;
        return message.substr(open + 1, close - open - 1);
    }

    std::vector<CLspDiagnostic> pending_undeclared_;
    std::unordered_set<std::string> unplaceable_names_;
    // Raised while walking a body whose declarations came from a macro
    // this server never expanded, where nothing is placeable by
    // construction.
    int suppress_undeclared_ = 0;

    // --- Individual checks --------------------------------------------

    /** @brief Reports a duplicated member name inside every struct or union a declaration defines. */
    void CheckRecordDefinitions(const CNode *decl) {
        for (const CNodePtr &kid : decl->kids) {
            if (kid->kind == CNodeKind::RecordDecl) CheckRecordMembers(kid.get());
            if (kid->kind == CNodeKind::EnumDecl) CheckEnumerators(kid.get());
        }
    }

    /** @brief Reports duplicated member names in one struct or union. */
    void CheckRecordMembers(const CNode *rec) {
        std::unordered_map<std::string, int> seen;
        for (const CNodePtr &field : rec->body) {
            for (const CNodePtr &kid : field->kids) {
                if (kid->kind == CNodeKind::RecordDecl) CheckRecordMembers(kid.get());
            }
            if (field->kind != CNodeKind::Field || field->name.empty()) continue;
            const auto it = seen.find(field->name);
            if (it != seen.end()) {
                ReportName(field.get(), CLspSeverity::Error, "duplicate-member",
                           "`" + field->name + "` is already a member, on line " + std::to_string(it->second + 1));
            } else {
                seen[field->name] = field->name_pos.line;
            }
        }
    }

    /** @brief Reports duplicated enumeration constant names in one enum. */
    void CheckEnumerators(const CNode *en) {
        std::unordered_map<std::string, int> seen;
        for (const CNodePtr &e : en->body) {
            if (e->kind != CNodeKind::Enumerator || e->name.empty()) continue;
            const auto it = seen.find(e->name);
            if (it != seen.end()) {
                ReportName(e.get(), CLspSeverity::Error, "duplicate-enumerator",
                           "`" + e->name + "` is already a constant of this enum, on line " +
                               std::to_string(it->second + 1));
            } else {
                seen[e->name] = e->name_pos.line;
            }
        }
    }

    /**
     * @brief Reports an assignment written where a comparison was meant.
     *
     * The convention C programmers actually use -- a second pair of
     * parentheses around a deliberate assignment -- is what this
     * respects, so `while ((c = getchar()) != EOF)` and
     * `if ((fd = open(p, O_RDONLY)) < 0)` stay silent and `if (x = 0)`
     * does not.
     */
    void CheckCondition(const CNode *cond, const char *keyword) {
        if (cond == nullptr) return;
        if (cond->kind != CNodeKind::Assign || cond->name != "=") return;
        Report(cond->name_pos.line, cond->name_pos.col, cond->name_pos.col + 1, CLspSeverity::Warning,
               "assign-in-condition",
               std::string("This `") + keyword + "` condition assigns, it does not compare. Write `==` to compare, "
               "or wrap it in another pair of parentheses to say the assignment is deliberate");
    }

    /** @brief Reports `if (x);`, where the body someone meant to write is the next line. */
    void CheckEmptyBody(const CNode *stmt) {
        if (stmt->body.empty() || stmt->body.front() == nullptr) return;
        const CNode *body = stmt->body.front().get();
        if (body->kind != CNodeKind::EmptyStmt) return;
        // A `while (...) ;` is a real busy-wait idiom; an `if (...) ;`
        // never is.
        Report(body->start.line, body->start.col, body->start.col + 1, CLspSeverity::Warning, "empty-if-body",
               "This `if` does nothing: the `;` right after the condition is its whole body");
    }

    /**
     * @brief Reports `a & b == c`, which C parses as `a & (b == c)`.
     *
     * The single most reliable bug C's precedence table produces: the
     * bitwise operators bind looser than the comparisons, which is the
     * opposite of what everyone reads. Only flagged when the comparison
     * is the *right* operand, since `(a & b) == c` written out is fine.
     */
    void CheckBitwisePrecedence(const CNode *expr) {
        if (expr->name != "&" && expr->name != "|" && expr->name != "^") return;
        if (expr->kids.size() < 2) return;
        const CNode *rhs = expr->kids[1].get();
        if (rhs->kind != CNodeKind::Binary) return;
        const std::string &op = rhs->name;
        if (op != "==" && op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">=") return;
        Report(expr->name_pos.line, expr->name_pos.col, expr->name_end_col, CLspSeverity::Warning,
               "bitwise-precedence",
               "`" + expr->name + "` binds looser than `" + op + "`, so this reads as `a " + expr->name + " (b " +
                   op + " c)`. Parenthesize the `" + expr->name + "` if that is not what you meant");
    }

    /** @brief Reports `x = x;`, which is always either a typo or a line someone forgot to finish. */
    void CheckSelfAssignment(const CNode *expr) {
        if (expr->name != "=" || expr->kids.size() < 2) return;
        const std::string lhs = RenderSimpleExpression(expr->kids[0].get());
        if (lhs.empty()) return;
        if (lhs != RenderSimpleExpression(expr->kids[1].get())) return;
        Report(expr->name_pos.line, expr->name_pos.col, expr->name_pos.col + 1, CLspSeverity::Warning,
               "self-assignment", "`" + lhs + "` is assigned to itself, which does nothing");
    }

    /** @brief Renders the simple lvalue shapes self-assignment compares, or "" for anything else. */
    static std::string RenderSimpleExpression(const CNode *expr) {
        if (expr == nullptr) return std::string();
        switch (expr->kind) {
            case CNodeKind::Ident:
                return expr->name;
            case CNodeKind::Paren:
                return expr->kids.empty() ? std::string() : RenderSimpleExpression(expr->kids.front().get());
            case CNodeKind::Member:
            case CNodeKind::Arrow: {
                if (expr->kids.empty() || expr->name.empty()) return std::string();
                const std::string base = RenderSimpleExpression(expr->kids.front().get());
                if (base.empty()) return std::string();
                return base + (expr->kind == CNodeKind::Arrow ? "->" : ".") + expr->name;
            }
            default:
                return std::string();
        }
    }

    /** @brief Reports a character constant holding more than one character, whose value is implementation-defined. */
    void CheckCharLiteral(const CNode *expr) {
        const std::string &raw = expr->name;
        const size_t open = raw.find('\'');
        if (open == std::string::npos) return;
        size_t count = 0;
        for (size_t i = open + 1; i < raw.size() && raw[i] != '\''; i++) {
            count++;
            if (raw[i] != '\\') continue;
            // One escape is one character, however long it is written:
            // `\x20`, `\033` and `\u2510` are all a single constant and
            // none of them is the mistake this check is looking for.
            i++;
            if (i >= raw.size()) break;
            const char kind = raw[i];
            if (kind == 'x') {
                while (i + 1 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1])) != 0) i++;
            } else if (kind == 'u' || kind == 'U') {
                const size_t want = kind == 'u' ? 4 : 8;
                for (size_t k = 0; k < want && i + 1 < raw.size() &&
                                   std::isxdigit(static_cast<unsigned char>(raw[i + 1])) != 0;
                     k++) {
                    i++;
                }
            } else if (kind >= '0' && kind <= '7') {
                for (size_t k = 0; k < 2 && i + 1 < raw.size() && raw[i + 1] >= '0' && raw[i + 1] <= '7'; k++) i++;
            }
        }
        if (count <= 1) return;
        Report(expr->start.line, expr->start.col, expr->end.col, CLspSeverity::Warning, "multi-character-constant",
               "A character constant with " + std::to_string(count) +
                   " characters in it has an implementation-defined value. Did you mean double quotes?");
    }

    /** @brief Reports duplicated `case` labels and a second `default` in one switch. */
    void CheckSwitch(const CNode *stmt) {
        std::map<long long, int> values;
        bool saw_default = false;
        int default_line = 0;
        CollectCases(stmt->body.empty() ? nullptr : stmt->body.front().get(), &values, &saw_default, &default_line);
    }

    /** @brief Walks a switch body's own statements (never a nested switch), reporting duplicate labels. */
    void CollectCases(const CNode *stmt, std::map<long long, int> *values, bool *saw_default, int *default_line) {
        if (stmt == nullptr) return;
        if (stmt->kind == CNodeKind::SwitchStmt) return;  // the inner switch checks itself
        if (stmt->kind == CNodeKind::CaseStmt && !stmt->kids.empty()) {
            long long value = 0;
            if (stmt->kids.size() == 1 && EvaluateConstant(stmt->kids.front().get(), &value)) {
                const auto it = values->find(value);
                if (it != values->end()) {
                    Report(stmt->start.line, stmt->start.col, stmt->start.col + 4, CLspSeverity::Error,
                           "duplicate-case",
                           "This switch already has a `case " + std::to_string(value) + ":`, on line " +
                               std::to_string(it->second + 1));
                } else {
                    (*values)[value] = stmt->start.line;
                }
            }
        }
        if (stmt->kind == CNodeKind::DefaultStmt) {
            if (*saw_default) {
                Report(stmt->start.line, stmt->start.col, stmt->start.col + 7, CLspSeverity::Error,
                       "duplicate-default",
                       "This switch already has a `default:`, on line " + std::to_string(*default_line + 1));
            } else {
                *saw_default = true;
                *default_line = stmt->start.line;
            }
        }
        for (const CNodePtr &kid : stmt->body) CollectCases(kid.get(), values, saw_default, default_line);
    }

    /** @brief Evaluates the constant expressions a `case` label realistically holds, or reports failure. */
    bool EvaluateConstant(const CNode *expr, long long *out) const {
        if (expr == nullptr) return false;
        switch (expr->kind) {
            case CNodeKind::Number: {
                const std::string &text = expr->name;
                if (text.find('.') != std::string::npos) return false;
                char *end = nullptr;
                const long long value = std::strtoll(text.c_str(), &end, 0);
                if (end == text.c_str()) return false;
                *out = value;
                return true;
            }
            case CNodeKind::CharLit: {
                const size_t open = expr->name.find('\'');
                if (open == std::string::npos || open + 1 >= expr->name.size()) return false;
                const char c = expr->name[open + 1];
                if (c == '\\') {
                    if (open + 2 >= expr->name.size()) return false;
                    switch (expr->name[open + 2]) {
                        case 'n': *out = '\n'; return true;
                        case 't': *out = '\t'; return true;
                        case 'r': *out = '\r'; return true;
                        case '0': *out = 0; return true;
                        default: *out = static_cast<unsigned char>(expr->name[open + 2]); return true;
                    }
                }
                *out = static_cast<unsigned char>(c);
                return true;
            }
            case CNodeKind::Paren:
                return !expr->kids.empty() && EvaluateConstant(expr->kids.front().get(), out);
            case CNodeKind::Unary: {
                if (expr->kids.empty() || expr->str_value == "post") return false;
                long long inner = 0;
                if (!EvaluateConstant(expr->kids.front().get(), &inner)) return false;
                if (expr->name == "-") {
                    *out = -inner;
                    return true;
                }
                if (expr->name == "+") {
                    *out = inner;
                    return true;
                }
                if (expr->name == "~") {
                    *out = ~inner;
                    return true;
                }
                return false;
            }
            default:
                return false;
        }
    }

    /** @brief Records every label a function body defines, so a `goto` can be checked against them. */
    void CollectLabels(const std::vector<CNodePtr> &body) {
        for (const CNodePtr &stmt : body) CollectLabelsIn(stmt.get());
    }

    /** @brief Records the labels inside one statement, recursively. */
    void CollectLabelsIn(const CNode *stmt) {
        if (stmt == nullptr) return;
        if (stmt->kind == CNodeKind::LabelStmt && !stmt->name.empty()) {
            Symbol sym;
            sym.name = stmt->name;
            sym.kind = SymKind::Label;
            sym.pos = stmt->name_pos;
            sym.name_end_col = stmt->name_end_col;
            sym.node = stmt;
            labels_.push_back(std::move(sym));
        }
        for (const CNodePtr &kid : stmt->body) CollectLabelsIn(kid.get());
        for (const CNodePtr &kid : stmt->orelse) CollectLabelsIn(kid.get());
        for (const CNodePtr &kid : stmt->kids) {
            if (kid != nullptr && kid->kind == CNodeKind::DeclStmt) continue;
            CollectLabelsIn(kid.get());
        }
    }

    /** @brief Reports a `goto` with no matching label, and a label nothing jumps to. */
    void CheckLabels() {
        for (const auto &target : gotos_) {
            bool found = false;
            for (Symbol &label : labels_) {
                if (label.name != target.first) continue;
                label.used = true;
                found = true;
            }
            if (found) continue;
            Report(target.second.line, target.second.col,
                   target.second.col + static_cast<int>(target.first.size()), CLspSeverity::Error, "undefined-label",
                   "No label named `" + target.first + "` in this function");
        }
        for (const Symbol &label : labels_) {
            if (label.used || mentioned_elsewhere_.count(label.name) > 0) continue;
            Report(label.pos.line, label.pos.col, label.name_end_col, CLspSeverity::Warning, "unused-label",
                   "Nothing jumps to `" + label.name + "`");
        }
    }

    /** @brief Reports the return statements that do not match the function's declared return type. */
    void CheckReturns(const CNode *fn) {
        // `RTDECL(void) f(...)` and `DECLINLINE(int) g(...)` declare
        // their return type inside a macro, so this server has no idea
        // what it is. Saying "declared to return `RTDECL`" would be
        // nonsense, and guessing would be worse.
        if (!IsReadableType(fn->type)) return;
        const bool returns_void = fn->type.IsVoid();
        bool saw_value = false;
        std::vector<const CNode *> returns;
        CollectReturns(fn->body, &returns);
        for (const CNode *ret : returns) {
            const bool has_value = !ret->kids.empty() && ret->kids.front()->kind != CNodeKind::Placeholder;
            if (has_value) saw_value = true;
            if (returns_void && has_value) {
                Report(ret->start.line, ret->start.col, ret->start.col + 6, CLspSeverity::Error,
                       "return-value-in-void", "`" + fn->name + "` returns `void`, so this cannot return a value");
            }
            if (!returns_void && !has_value) {
                Report(ret->start.line, ret->start.col, ret->start.col + 6, CLspSeverity::Warning, "return-no-value",
                       "`" + fn->name + "` is declared to return `" + fn->type.spelling +
                           "`, but this returns nothing");
            }
        }
        if (returns_void || saw_value || fn->body.empty()) return;
        if ((fn->flags & kCFlagNoreturn) != 0) return;
        if (EndsWithNoReturnCall(fn->body)) return;
        // `main` is allowed to fall off the end; every other function
        // that does leaves its caller reading uninitialized memory.
        if (fn->name == "main") return;
        Report(fn->name_pos.line, fn->name_pos.col, fn->name_end_col, CLspSeverity::Warning, "missing-return",
               "`" + fn->name + "` is declared to return `" + fn->type.spelling + "` but never returns a value");
    }

    /**
     * @brief Reports whether a type is one this server can name, rather than a macro standing in for one.
     *
     * A base that is a type keyword, a tag, a typedef this file or a
     * resolved header introduced, or a standard type name is readable.
     * Anything else came out of the preprocessor and is not this
     * server's to reason about.
     */
    bool IsReadableType(const CType &type) const {
        if (type.base.empty()) return true;  // an implicit `int`
        if (type.is_struct) return true;
        const size_t space = type.base.find(' ');
        const std::string first = space == std::string::npos ? type.base : type.base.substr(0, space);
        if (IsCTypeKeyword(first)) return true;
        if (typedefs_.count(type.base) > 0) return true;
        const auto it = globals_.find(type.base);
        if (it != globals_.end() && symbols_[it->second].kind == SymKind::Typedef) return true;
        return IsStandardName(type.base);
    }

    /** @brief Collects every `return` in a body, not descending into nested function-shaped nodes. */
    void CollectReturns(const std::vector<CNodePtr> &body, std::vector<const CNode *> *out) {
        for (const CNodePtr &stmt : body) CollectReturnsIn(stmt.get(), out);
    }

    /** @brief Collects the `return` statements inside one statement. */
    void CollectReturnsIn(const CNode *stmt, std::vector<const CNode *> *out) {
        if (stmt == nullptr) return;
        if (stmt->kind == CNodeKind::ReturnStmt) out->push_back(stmt);
        for (const CNodePtr &kid : stmt->body) CollectReturnsIn(kid.get(), out);
        for (const CNodePtr &kid : stmt->orelse) CollectReturnsIn(kid.get(), out);
        for (const CNodePtr &kid : stmt->kids) CollectReturnsIn(kid.get(), out);
    }

    /** @brief Reports whether a body ends in a call that never comes back (`exit`, `abort`, `longjmp`). */
    static bool EndsWithNoReturnCall(const std::vector<CNodePtr> &body) {
        for (size_t i = body.size(); i > 0; i--) {
            const CNode *stmt = body[i - 1].get();
            if (stmt == nullptr) continue;
            if (stmt->kind == CNodeKind::CompoundStmt) return EndsWithNoReturnCall(stmt->body);
            if (stmt->kind == CNodeKind::ExprStmt && !stmt->kids.empty()) {
                const CNode *call = stmt->kids.front().get();
                if (call->kind != CNodeKind::Call) return false;
                const std::string &name = call->name;
                return name == "exit" || name == "_exit" || name == "_Exit" || name == "abort" ||
                       name == "longjmp" || name == "siglongjmp" || name == "err" || name == "errx" ||
                       name == "panic" || name == "unreachable" || name == "__builtin_unreachable" ||
                       name == "pthread_exit" || name == "assert";
            }
            // A `while (1) { ... }` with no break never falls out either.
            if (stmt->kind == CNodeKind::WhileStmt || stmt->kind == CNodeKind::ForStmt ||
                stmt->kind == CNodeKind::DoStmt) {
                return true;
            }
            if (stmt->kind == CNodeKind::SwitchStmt) return true;
            return false;
        }
        return false;
    }

    /** @brief Reports an expression statement whose value is computed and thrown away. */
    void CheckStatementHasEffect(const CNode *expr) {
        if (expr == nullptr || HasEffect(expr)) return;
        // `Py_BEGIN_ALLOW_THREADS;` is a macro that expands to real
        // statements. A bare name that resolves to nothing this file
        // declares is one of those, not a variable evaluated for
        // nothing -- and `x;` for a real `x` still reports.
        if (expr->kind == CNodeKind::Ident && Lookup(expr->name) == nullptr && FindGlobal(expr->name) == nullptr) {
            return;
        }
        Report(expr->start.line, expr->start.col, expr->end.col, CLspSeverity::Warning, "statement-no-effect",
               "This statement computes a value and then discards it");
    }

    /** @brief Reports whether an expression does something other than produce a value. */
    static bool HasEffect(const CNode *expr) {
        if (expr == nullptr) return true;
        switch (expr->kind) {
            case CNodeKind::Assign:
            case CNodeKind::Call:
            case CNodeKind::StmtExpr:
            case CNodeKind::Placeholder:
            case CNodeKind::Generic:
                return true;
            case CNodeKind::Unary:
                // `++i` and `i--` do; `*p;` and `-x;` do not. `&&label`
                // is a GNU address-of-label, which only appears where it
                // is used.
                return expr->name == "++" || expr->name == "--" || expr->name == "&&";
            case CNodeKind::Comma:
                for (const CNodePtr &kid : expr->kids) {
                    if (HasEffect(kid.get())) return true;
                }
                return false;
            case CNodeKind::Conditional:
                for (size_t i = 1; i < expr->kids.size(); i++) {
                    if (HasEffect(expr->kids[i].get())) return true;
                }
                return false;
            case CNodeKind::Paren:
                return expr->kids.empty() || HasEffect(expr->kids.front().get());
            case CNodeKind::Cast:
                // `(void)x;` is how C says "I know, and I mean it".
                if (expr->kids.size() >= 2 && expr->kids[0]->type.IsVoid()) return true;
                return expr->kids.size() >= 2 && HasEffect(expr->kids[1].get());
            case CNodeKind::Ident:
            case CNodeKind::Number:
            case CNodeKind::StrLit:
            case CNodeKind::CharLit:
            case CNodeKind::Member:
            case CNodeKind::Arrow:
            case CNodeKind::Index:
            case CNodeKind::Binary:
            case CNodeKind::SizeOfExpr:
            case CNodeKind::SizeOfType:
            case CNodeKind::AlignOf:
                return false;
            default:
                return true;
        }
    }

    /** @brief Checks a member access against the struct it is reaching into, when that struct is known. */
    void CheckMember(const CNode *expr) {
        if (expr->name.empty() || expr->kids.empty()) return;
        if (!ShouldReportUndeclared()) return;
        const CType object = TypeOf(expr->kids.front().get());
        if (object.base.empty()) return;
        const CNode *rec = RecordOf(object);
        if (rec == nullptr || rec->body.empty()) return;
        if (FindField(rec, expr->name) != nullptr) return;
        // An anonymous member's fields belong to the outer struct too,
        // and this server does not follow them; stay quiet rather than
        // guess.
        for (const CNodePtr &field : rec->body) {
            if (field->kind == CNodeKind::Field && field->name.empty()) return;
            if (field->kind == CNodeKind::RecordDecl) return;
        }
        ReportName(expr, CLspSeverity::Error, "no-such-member",
                   "`" + ResolveTypedef(object).base + "` has no member named `" + expr->name + "`");
    }

    /** @brief Finds a field by name in a struct or union declaration, or nullptr. */
    static const CNode *FindField(const CNode *rec, const std::string &name) {
        for (const CNodePtr &field : rec->body) {
            if (field->kind == CNodeKind::Field && field->name == name) return field.get();
        }
        return nullptr;
    }

    /** @brief Works out an expression's type, as far as this server's shallow model goes. */
    CType TypeOf(const CNode *expr) {
        if (expr == nullptr) return CType();
        switch (expr->kind) {
            case CNodeKind::Ident: {
                const Symbol *sym = Lookup(expr->name);
                if (sym == nullptr) sym = FindGlobal(expr->name);
                return sym == nullptr ? CType() : sym->type;
            }
            case CNodeKind::Paren:
                return expr->kids.empty() ? CType() : TypeOf(expr->kids.front().get());
            case CNodeKind::Cast:
            case CNodeKind::CompoundLiteral:
                return expr->kids.empty() ? CType() : expr->kids.front()->type;
            case CNodeKind::Member:
            case CNodeKind::Arrow: {
                if (expr->kids.empty()) return CType();
                const CNode *rec = RecordOf(TypeOf(expr->kids.front().get()));
                if (rec == nullptr) return CType();
                const CNode *field = FindField(rec, expr->name);
                return field == nullptr ? CType() : field->type;
            }
            case CNodeKind::Index: {
                if (expr->kids.empty()) return CType();
                CType base = TypeOf(expr->kids.front().get());
                if (base.array_dims > 0) base.array_dims--;
                else if (base.pointers > 0) base.pointers--;
                return base;
            }
            case CNodeKind::Unary: {
                if (expr->kids.empty()) return CType();
                CType base = TypeOf(expr->kids.front().get());
                if (expr->name == "*" && base.pointers > 0) base.pointers--;
                if (expr->name == "*" && base.pointers == 0 && base.array_dims > 0) base.array_dims--;
                if (expr->name == "&") base.pointers++;
                return base;
            }
            case CNodeKind::Call: {
                const Symbol *sym = expr->name.empty() ? nullptr : Lookup(expr->name);
                if (sym == nullptr && !expr->name.empty()) sym = FindGlobal(expr->name);
                if (sym == nullptr) return CType();
                CType t = sym->type;
                t.is_function = false;
                return t;
            }
            default:
                return CType();
        }
    }

    /** @brief Checks one call: the format-string family's argument count, and nothing else. */
    void CheckCall(const CNode *call) {
        if (call->name.empty() || call->kids.empty()) return;
        CheckFormatArguments(call);
    }

    /**
     * @brief Reports a `printf`-family call whose argument count does not match its format string.
     *
     * The one check here that reaches past a single expression, and it
     * earns it: a mismatched `printf` reads whatever happens to be in
     * the next register, the compiler is silent about it unless asked,
     * and the format string is right there to count. Only literal
     * formats are counted -- a format built from a macro or a variable
     * is skipped, not guessed at -- and positional `%n$` conversions
     * switch the check off, since their argument count is not the
     * conversion count.
     */
    void CheckFormatArguments(const CNode *call) {
        const int format_index = FormatArgumentIndex(call->name);
        if (format_index < 0) return;
        // kids[0] is the callee, so argument n is kids[n + 1].
        const size_t format_at = static_cast<size_t>(format_index) + 1;
        if (call->kids.size() <= format_at) return;
        const CNode *format = call->kids[format_at].get();
        if (format->kind != CNodeKind::StrLit) return;
        if ((format->flags & kCFlagAttrUnused) != 0) return;  // part of it came from a macro
        int expected = 0;
        if (!CountConversions(format->str_value, IsScanFamily(call->name), &expected)) return;
        const int given = static_cast<int>(call->kids.size() - format_at - 1);
        if (given == expected) return;
        const std::string what = expected == 1 ? "1 argument" : std::to_string(expected) + " arguments";
        const std::string got = given == 1 ? "1 was" : std::to_string(given) + " were";
        Report(format->start.line, format->start.col, format->end.col, CLspSeverity::Warning, "format-argument-count",
               "This format asks for " + what + ", but " + got + " passed");
    }

    /** @brief Reports whether a formatting function reads rather than writes, which changes what `*` means. */
    static bool IsScanFamily(const std::string &name) {
        return name.find("scanf") != std::string::npos;
    }

    /** @brief The 0-based argument position of a formatting function's format string, or -1. */
    static int FormatArgumentIndex(const std::string &name) {
        if (name == "printf" || name == "scanf" || name == "wprintf" || name == "wscanf") return 0;
        if (name == "fprintf" || name == "sprintf" || name == "dprintf" || name == "fscanf" || name == "sscanf" ||
            name == "asprintf" || name == "fwprintf" || name == "swscanf" || name == "syslog") {
            return 1;
        }
        if (name == "snprintf" || name == "swprintf") return 2;
        return -1;
    }

    /**
     * @brief Counts the arguments a format string asks for.
     * @return false when the format uses something this check refuses to guess at
     */
    static bool CountConversions(const std::string &format, bool scanning, int *out) {
        int count = 0;
        for (size_t i = 0; i < format.size(); i++) {
            if (format[i] != '%') continue;
            i++;
            if (i >= format.size()) return false;  // a trailing `%` is its own problem
            if (format[i] == '%') continue;
            // In the scanf family a `*` right after the `%` suppresses
            // the assignment, so the conversion takes no argument at
            // all; in the printf family the same character asks for one
            // more. Getting this backwards makes every `%*[^\n]` a
            // false report.
            bool suppressed = false;
            if (scanning && format[i] == '*') {
                suppressed = true;
                i++;
            }
            // Flags.
            while (i < format.size() && std::strchr("-+ #0'", format[i]) != nullptr) i++;
            // Width, which in the printf family may itself be an argument.
            if (!scanning && i < format.size() && format[i] == '*') {
                count++;
                i++;
            } else {
                const size_t digits_at = i;
                while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])) != 0) i++;
                // A positional `%2$s`: the count is no longer the number
                // of conversions, so this check has nothing to say.
                if (i < format.size() && format[i] == '$' && i > digits_at) return false;
            }
            // Precision.
            if (i < format.size() && format[i] == '.') {
                i++;
                if (i < format.size() && format[i] == '*') {
                    count++;
                    i++;
                } else {
                    while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])) != 0) i++;
                }
            }
            // Length modifier. `m` is one in the scanf family (allocate
            // the result) and a whole conversion in the printf family
            // (the text of errno), so it belongs on different sides of
            // this line depending on which we are reading.
            while (i < format.size() && std::strchr("hlLqjzt", format[i]) != nullptr) i++;
            if (scanning) {
                while (i < format.size() && (format[i] == 'm' || format[i] == 'a')) i++;
            }
            if (i >= format.size()) return false;
            // A scanset: `%[^\n]`, `%[]abc]`. Everything up to the
            // closing bracket is the conversion, `%` included.
            if (format[i] == '[') {
                i++;
                if (i < format.size() && format[i] == '^') i++;
                if (i < format.size() && format[i] == ']') i++;
                while (i < format.size() && format[i] != ']') i++;
                if (i >= format.size()) return false;  // an unclosed scanset
                if (!suppressed) count++;
                continue;
            }
            if (!scanning && format[i] == 'm') continue;  // glibc's `%m`, which takes no argument
            if (!suppressed) count++;
        }
        *out = count;
        return true;
    }

    /** @brief Reports the file-scope names with internal linkage that nothing in this file uses. */
    void ReportUnusedFileScope() {
        // A header declares things for other files to use; nothing in it
        // is dead just because this file does not call it.
        if (in_header_) return;
        for (Symbol &sym : symbols_) {
            if (sym.scope != 0 || !sym.path.empty() || sym.used || sym.reported) continue;
            if ((sym.flags & (kCFlagStatic)) == 0) continue;  // external linkage: another file may use it
            if ((sym.flags & kCFlagAttrUnused) != 0) continue;
            if (sym.kind != SymKind::Function && sym.kind != SymKind::Variable) continue;
            if (mentioned_elsewhere_.count(sym.name) > 0) continue;
            sym.reported = true;
            const char *what = sym.kind == SymKind::Function ? "function" : "variable";
            Report(sym.pos.line, sym.pos.col, sym.name_end_col, CLspSeverity::Warning,
                   sym.kind == SymKind::Function ? "unused-static-function" : "unused-static-variable",
                   std::string("`") + sym.name + "` is a `static` " + what +
                       " that nothing in this file uses, so nothing can");
        }
    }

    /** @brief Reports the lines past the configured length, when a caller asked for that at all. */
    void ReportLineLengths() {
        if (opts_.max_line_length <= 0) return;
        for (size_t i = 0; i < lines_.size(); i++) {
            const int length = static_cast<int>(lines_[i].size());
            if (length <= opts_.max_line_length) continue;
            Report(static_cast<int>(i), opts_.max_line_length, length, CLspSeverity::Hint, "line-too-long",
                   "This line is " + std::to_string(length) + " columns wide; the limit here is " +
                       std::to_string(opts_.max_line_length));
        }
    }

    // --- Position-aware lookup, for the interactive features ----------
    //
    // Everything below is public: the interactive features drive the
    // same collection the diagnostics do, but from outside the class,
    // one cursor position at a time.
public:
    /** @brief Finds the function definition whose body contains a line, or nullptr. */
    const CNode *EnclosingFunction(int line) const {
        for (const CNodePtr &node : parse_.unit->body) {
            if (node->kind != CNodeKind::FunctionDef && node->kind != CNodeKind::MacroDecl) continue;
            if (node->start.line <= line && line <= node->end.line) return node.get();
        }
        return nullptr;
    }

    /**
     * @brief Pushes the locals visible at a position onto the scope stack.
     *
     * Deliberately generous about block nesting: every declaration in
     * the enclosing function that starts before the cursor counts,
     * whether or not its block is still open. Offering a name from a
     * sibling block costs a spurious completion; hiding one costs the
     * completion someone was reaching for, and mep's other servers make
     * the same trade.
     */
    void PushLocalsAt(const CNode *fn, int line, int col) {
        PushScope();
        if (fn == nullptr) return;
        for (const CNodePtr &param : fn->params) {
            if (param->name.empty() || param->name == "...") continue;
            Symbol sym;
            sym.name = param->name;
            sym.kind = SymKind::Parameter;
            sym.type = param->type;
            sym.pos = param->name_pos;
            sym.name_end_col = param->name_end_col;
            sym.node = param.get();
            sym.used = true;
            sym.detail = DescribeParameter(param.get());
            Declare(std::move(sym));
        }
        CollectLocalsBefore(fn->body, line, col);
    }

    /** @brief Declares every local a body introduces before a position. */
    void CollectLocalsBefore(const std::vector<CNodePtr> &body, int line, int col) {
        for (const CNodePtr &stmt : body) CollectLocalsBeforeIn(stmt.get(), line, col);
    }

    /** @brief Declares the locals one statement introduces before a position. */
    void CollectLocalsBeforeIn(const CNode *stmt, int line, int col) {
        if (stmt == nullptr) return;
        if (stmt->start.line > line) return;
        if (stmt->kind == CNodeKind::DeclStmt && !stmt->kids.empty()) {
            const CNode *decl = stmt->kids.front().get();
            for (const CNodePtr &d : decl->kids) {
                if (d->kind != CNodeKind::Declarator || d->name.empty()) continue;
                if (d->name_pos.line > line || (d->name_pos.line == line && d->name_pos.col > col)) continue;
                Symbol sym;
                sym.name = d->name;
                sym.kind = (decl->flags & kCFlagTypedef) != 0 ? SymKind::Typedef : SymKind::Variable;
                sym.type = d->type;
                sym.pos = d->name_pos;
                sym.name_end_col = d->name_end_col;
                sym.flags = decl->flags | d->flags;
                sym.node = d.get();
                sym.used = true;
                sym.detail = DescribeDeclaration(d.get());
                Declare(std::move(sym));
            }
            RegisterTags(decl, std::string());
            for (const CNodePtr &d : decl->kids) {
                if (d->kind == CNodeKind::Declarator && (decl->flags & kCFlagTypedef) != 0 && !d->name.empty()) {
                    typedefs_[d->name] = d->type;
                }
            }
        }
        for (const CNodePtr &kid : stmt->body) CollectLocalsBeforeIn(kid.get(), line, col);
        for (const CNodePtr &kid : stmt->orelse) CollectLocalsBeforeIn(kid.get(), line, col);
        for (const CNodePtr &kid : stmt->kids) CollectLocalsBeforeIn(kid.get(), line, col);
    }

    /** @brief The symbols visible from the innermost scope outwards, innermost first. */
    std::vector<const Symbol *> VisibleSymbols() const {
        std::vector<const Symbol *> out;
        std::unordered_set<std::string> seen;
        for (size_t i = scopes_.size(); i > 0; i--) {
            for (const auto &entry : scopes_[i - 1]) {
                if (!seen.insert(symbols_[entry.second].name).second) continue;
                out.push_back(&symbols_[entry.second]);
            }
        }
        for (const auto &entry : globals_) {
            if (!seen.insert(symbols_[entry.second].name).second) continue;
            out.push_back(&symbols_[entry.second]);
        }
        return out;
    }

    /** @brief Finds a symbol by name from the innermost scope outwards, or nullptr. */
    const Symbol *LookupConst(const std::string &name) { return Lookup(name); }

    /** @brief Works out an expression's type (public form, for the feature half). */
    CType TypeOfPublic(const CNode *expr) { return TypeOf(expr); }

    /** @brief Renders a declaration the way hover wants to read it (public form). */
    static std::string Describe(const CNode *node) { return DescribeDeclaration(node); }

    /** @brief Renders a parameter for a signature line (public form). */
    static std::string DescribeParam(const CNode *node) { return DescribeParameter(node); }

    /** @brief Finds a field by name in a struct or union declaration (public form), or nullptr. */
    static const CNode *Field(const CNode *rec, const std::string &name) { return FindField(rec, name); }
};

}  // namespace

// --- Shared cursor helpers -------------------------------------------

namespace {

// The identifier a cursor sits on or just after, with its span.
struct WordAtCursor {
    std::string text;
    int start = 0;
    int end = 0;
    bool found = false;
};

/**
 * @brief Finds the identifier at a cursor position.
 * @param lines the document
 * @param line 0-based line
 * @param col 0-based byte column
 * @param include_following whether to extend past the cursor to the end of the word (hover wants this; completion does not)
 */
WordAtCursor WordAt(const std::vector<std::string> &lines, int line, int col, bool include_following) {
    WordAtCursor word;
    const std::string &text = LineAt(lines, line);
    int at = col;
    if (at > static_cast<int>(text.size())) at = static_cast<int>(text.size());
    int start = at;
    while (start > 0 && IsWordChar(text[static_cast<size_t>(start) - 1])) start--;
    int end = at;
    if (include_following) {
        while (end < static_cast<int>(text.size()) && IsWordChar(text[static_cast<size_t>(end)])) end++;
    }
    if (start == end) return word;
    word.text = text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
    word.start = start;
    word.end = end;
    word.found = true;
    return word;
}

/** @brief Reports whether a position sits inside a string, a character constant or a comment. */
bool InStringOrComment(const CParseResult &parse, int line, int col) {
    for (const CComment &c : parse.comments) {
        const bool after_start = line > c.line || (line == c.line && col > c.col);
        const bool before_end = line < c.end_line || (line == c.end_line && col <= c.end_col);
        if (after_start && before_end) return true;
    }
    for (const CToken &t : parse.tokens) {
        if (t.kind != CTokKind::String && t.kind != CTokKind::CharLit) continue;
        const bool after_start = line > t.start.line || (line == t.start.line && col > t.start.col);
        const bool before_end = line < t.end.line || (line == t.end.line && col < t.end.col);
        if (after_start && before_end) return true;
    }
    return false;
}

/** @brief Finds the directive whose line the cursor is on, or nullptr. */
const CDirective *DirectiveOnLine(const CParseResult &parse, int line) {
    for (const CDirective &d : parse.directives) {
        if (d.line <= line && line <= d.end_line) return &d;
    }
    return nullptr;
}

/** @brief Reports whether a prefix matches a candidate, case-insensitively for a leading-case mismatch. */
bool MatchesPrefix(const std::string &candidate, const std::string &prefix) {
    if (prefix.empty()) return true;
    if (candidate.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        const char a = candidate[i];
        const char b = prefix[i];
        if (a == b) continue;
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}

// One step of a member-access chain, read right to left off the source
// text: `frame->buf[i].len` is [Ident frame, Arrow buf, Index, Member len].
enum class ChainOp { Name, Member, Arrow, Index, Call };

struct ChainStep {
    ChainOp op = ChainOp::Name;
    std::string name;
};

/**
 * @brief Reads the postfix expression immediately before a `.` or `->` at `at`.
 *
 * Done on the text rather than on the tree on purpose: this runs while
 * someone is typing `p->`, which is the moment the tree is least likely
 * to have the shape the finished expression will. The chain it
 * understands -- names, members, subscripts and calls -- is the chain
 * people actually write in front of a `.`.
 */
std::vector<ChainStep> ReadChainBefore(const std::string &text, int at) {
    std::vector<ChainStep> steps;
    int i = at;
    const auto skip_space = [&] {
        while (i >= 0 && (text[static_cast<size_t>(i)] == ' ' || text[static_cast<size_t>(i)] == '\t')) i--;
    };
    for (int guard = 0; guard < 32; guard++) {
        skip_space();
        if (i < 0) break;
        const char c = text[static_cast<size_t>(i)];
        if (c == ']' || c == ')') {
            const char open = c == ']' ? '[' : '(';
            int depth = 0;
            while (i >= 0) {
                const char d = text[static_cast<size_t>(i)];
                if (d == c) depth++;
                if (d == open) {
                    depth--;
                    if (depth == 0) break;
                }
                i--;
            }
            i--;
            steps.push_back({c == ']' ? ChainOp::Index : ChainOp::Call, std::string()});
            continue;
        }
        if (!IsWordChar(c)) break;
        int end = i + 1;
        while (i >= 0 && IsWordChar(text[static_cast<size_t>(i)])) i--;
        ChainStep step;
        step.name = text.substr(static_cast<size_t>(i + 1), static_cast<size_t>(end - i - 1));
        skip_space();
        if (i >= 1 && text[static_cast<size_t>(i)] == '>' && text[static_cast<size_t>(i) - 1] == '-') {
            step.op = ChainOp::Arrow;
            i -= 2;
            steps.push_back(step);
            continue;
        }
        if (i >= 0 && text[static_cast<size_t>(i)] == '.' && (i == 0 || text[static_cast<size_t>(i) - 1] != '.')) {
            step.op = ChainOp::Member;
            i--;
            steps.push_back(step);
            continue;
        }
        step.op = ChainOp::Name;
        steps.push_back(step);
        break;
    }
    std::reverse(steps.begin(), steps.end());
    return steps;
}

/** @brief Walks a member chain through the type model, returning the type it ends at. */
CType ResolveChain(Analyzer &analyzer, const std::vector<ChainStep> &steps) {
    CType type;
    bool started = false;
    for (const ChainStep &step : steps) {
        switch (step.op) {
            case ChainOp::Name: {
                const Symbol *sym = analyzer.LookupConst(step.name);
                if (sym == nullptr) sym = analyzer.FindGlobal(step.name);
                if (sym == nullptr) return CType();
                type = sym->type;
                started = true;
                break;
            }
            case ChainOp::Member:
            case ChainOp::Arrow: {
                if (!started) return CType();
                const CNode *rec = analyzer.RecordOf(type);
                if (rec == nullptr) return CType();
                const CNode *field = Analyzer::Field(rec, step.name);
                if (field == nullptr) return CType();
                type = field->type;
                break;
            }
            case ChainOp::Index:
                if (type.array_dims > 0) type.array_dims--;
                else if (type.pointers > 0) type.pointers--;
                break;
            case ChainOp::Call:
                type.is_function = false;
                break;
        }
    }
    return started ? type : CType();
}

/** @brief Maps a symbol to the completion kind a client shows an icon for. */
CLspKind KindOfSymbol(const Symbol &sym) {
    switch (sym.kind) {
        case SymKind::Function: return CLspKind::Function;
        case SymKind::Parameter: return CLspKind::Variable;
        case SymKind::Typedef: return CLspKind::TypeParameter;
        case SymKind::EnumConstant: return CLspKind::EnumMember;
        case SymKind::Tag: return CLspKind::Struct;
        case SymKind::Macro: return CLspKind::Constant;
        default: return CLspKind::Variable;
    }
}

/** @brief Builds one completion item over the word being replaced. */
CLspCompletionItem MakeItem(const std::string &label, CLspKind kind, const std::string &detail,
                            const std::string &doc, int replace_start, int replace_end) {
    CLspCompletionItem item;
    item.label = label;
    item.insert_text = label;
    item.kind = kind;
    item.detail = detail;
    item.documentation = doc;
    item.replace_start = replace_start;
    item.replace_end = replace_end;
    return item;
}

/** @brief Renders a `#define` the way hover and completion show it. */
std::string DescribeMacro(const CDirective &d) {
    std::string out = "#define " + d.name;
    if (d.function_like) {
        out += "(";
        for (size_t i = 0; i < d.params.size(); i++) {
            if (i > 0) out += ", ";
            out += d.params[i];
        }
        out += ")";
    }
    if (!d.body.empty()) out += " " + d.body;
    return out;
}

}  // namespace

// --- Diagnostics entry point -----------------------------------------

std::vector<CLspDiagnostic> CLspDiagnostics(const std::vector<std::string> &lines, const CLspOptions &opts) {
    Analyzer analyzer(lines, opts);
    analyzer.Check();
    return analyzer.diagnostics();
}

// --- Completion -------------------------------------------------------

namespace {

/** @brief Offers the standard header names, and the sibling files, an `#include` line could name. */
std::vector<CLspCompletionItem> IncludeCompletions(const std::vector<std::string> &lines, int line, int col,
                                                   const CLspOptions &opts, bool angled) {
    std::vector<CLspCompletionItem> items;
    const std::string &text = LineAt(lines, line);
    // The part already typed runs from the opening bracket or quote to
    // the cursor, and is replaced whole -- a header name is not an
    // identifier, so the client's word-based replacement would cut it in
    // the middle of `sys/`.
    const size_t open = text.find_last_of(angled ? '<' : '"', static_cast<size_t>(col) == 0 ? 0 : static_cast<size_t>(col) - 1);
    if (open == std::string::npos) return items;
    const int start = static_cast<int>(open) + 1;
    if (start > col) return items;
    const std::string prefix = text.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    if (angled) {
        for (const CLspVocabEntry &entry : CLspHeaderVocab()) {
            const std::string name = entry.name;
            if (!MatchesPrefix(name, prefix)) continue;
            items.push_back(MakeItem(name, CLspKind::File, "standard header", std::string(), start, col));
        }
        return items;
    }
    if (!opts.check_files || opts.doc_dir.empty()) return items;
    namespace fs = std::filesystem;
    // A prefix may name a subdirectory, in which case that is what to list.
    const size_t slash = prefix.find_last_of('/');
    const std::string sub = slash == std::string::npos ? std::string() : prefix.substr(0, slash + 1);
    const std::string leaf = slash == std::string::npos ? prefix : prefix.substr(slash + 1);
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(fs::path(opts.doc_dir) / sub, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        const bool is_dir = entry.is_directory(ec);
        if (!is_dir) {
            const std::string ext = entry.path().extension().string();
            if (ext != ".h" && ext != ".inc" && ext != ".c") continue;
        }
        if (!MatchesPrefix(name, leaf)) continue;
        items.push_back(MakeItem(sub + name + (is_dir ? "/" : ""), is_dir ? CLspKind::Folder : CLspKind::File,
                                 is_dir ? "directory" : "header next to this file", std::string(), start, col));
    }
    return items;
}

/** @brief Offers the members of the struct or union a `.` or `->` reached into. */
std::vector<CLspCompletionItem> MemberCompletions(Analyzer &analyzer, const std::vector<std::string> &lines,
                                                  int line, int col, const WordAtCursor &word) {
    std::vector<CLspCompletionItem> items;
    const std::string &text = LineAt(lines, line);
    int at = word.found ? word.start : col;
    while (at > 0 && (text[static_cast<size_t>(at) - 1] == ' ' || text[static_cast<size_t>(at) - 1] == '\t')) at--;
    bool arrow = false;
    if (at >= 2 && text.compare(static_cast<size_t>(at) - 2, 2, "->") == 0) {
        arrow = true;
        at -= 2;
    } else if (at >= 1 && text[static_cast<size_t>(at) - 1] == '.' &&
               !(at >= 2 && text[static_cast<size_t>(at) - 2] == '.')) {
        at -= 1;
    } else {
        return items;
    }
    const std::vector<ChainStep> chain = ReadChainBefore(text, at - 1);
    if (chain.empty()) return items;
    CType type = ResolveChain(analyzer, chain);
    // `p->x` dereferences; `s.x` does not. Being strict about that would
    // mean refusing to complete `p.x` for a pointer `p`, which is a typo
    // the completion list is the best place to notice -- so the members
    // are offered either way and the type model stays quiet about it.
    (void)arrow;
    const CNode *rec = analyzer.RecordOf(type);
    if (rec == nullptr) return items;
    const int start = word.found ? word.start : col;
    for (const CNodePtr &field : rec->body) {
        if (field->kind != CNodeKind::Field || field->name.empty()) continue;
        if (word.found && !MatchesPrefix(field->name, word.text)) continue;
        items.push_back(MakeItem(field->name, CLspKind::Field, field->type.spelling, std::string(), start, col));
    }
    return items;
}

}  // namespace

std::vector<CLspCompletionItem> CLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                const CLspOptions &opts) {
    std::vector<CLspCompletionItem> items;
    Analyzer analyzer(lines, opts);
    analyzer.Collect();
    const CParseResult &parse = analyzer.parse();
    const std::string &text = LineAt(lines, line);
    const WordAtCursor word = WordAt(lines, line, col, false);
    const int start = word.found ? word.start : col;

    // Inside a directive line, the vocabulary is a different one.
    if (const CDirective *directive = DirectiveOnLine(parse, line); directive != nullptr) {
        const size_t hash = text.find('#');
        if (hash != std::string::npos && static_cast<int>(hash) < col) {
            const std::string before = text.substr(hash + 1, static_cast<size_t>(col) - hash - 1);
            // Still in the directive's own keyword.
            if (before.find_first_of(" \t") == std::string::npos) {
                for (const CLspVocabEntry &entry : CLspDirectiveVocab()) {
                    if (!MatchesPrefix(entry.name, word.found ? word.text : std::string())) continue;
                    items.push_back(MakeItem(entry.name, CLspKind::Keyword, entry.detail, entry.doc, start, col));
                }
                return items;
            }
        }
        if (directive->kind == CDirectiveKind::Include || directive->kind == CDirectiveKind::Embed) {
            const size_t angle = text.find('<');
            const size_t quote = text.find('"');
            const bool angled = angle != std::string::npos && (quote == std::string::npos || angle < quote);
            if (angle != std::string::npos || quote != std::string::npos) {
                return IncludeCompletions(lines, line, col, opts, angled);
            }
            return items;
        }
        if (directive->kind != CDirectiveKind::If && directive->kind != CDirectiveKind::Elif &&
            directive->kind != CDirectiveKind::Ifdef && directive->kind != CDirectiveKind::Ifndef &&
            directive->kind != CDirectiveKind::Undef && directive->kind != CDirectiveKind::Define) {
            return items;
        }
        // In a conditional or an `#undef`, the useful vocabulary is the
        // macros this file and its headers define.
        for (const auto &entry : analyzer.macros()) {
            if (!MatchesPrefix(entry.first, word.found ? word.text : std::string())) continue;
            items.push_back(MakeItem(entry.first, CLspKind::Constant, DescribeMacro(*entry.second), std::string(),
                                     start, col));
        }
        return items;
    }

    if (InStringOrComment(parse, line, col)) return items;

    analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);

    // A `.` or `->` in front of the cursor narrows everything down to one
    // struct's members.
    std::vector<CLspCompletionItem> members = MemberCompletions(analyzer, lines, line, col, word);
    if (!members.empty()) return members;

    // After `struct`, `union` or `enum`, only a tag can follow.
    {
        int at = start;
        while (at > 0 && (text[static_cast<size_t>(at) - 1] == ' ' || text[static_cast<size_t>(at) - 1] == '\t')) at--;
        int keyword_start = at;
        while (keyword_start > 0 && IsWordChar(text[static_cast<size_t>(keyword_start) - 1])) keyword_start--;
        const std::string keyword =
            text.substr(static_cast<size_t>(keyword_start), static_cast<size_t>(at - keyword_start));
        if (keyword == "struct" || keyword == "union" || keyword == "enum") {
            for (const auto &entry : analyzer.tags()) {
                const size_t space = entry.first.find(' ');
                if (space == std::string::npos) continue;
                if (entry.first.compare(0, space, keyword) != 0) continue;
                const std::string name = entry.first.substr(space + 1);
                if (word.found && !MatchesPrefix(name, word.text)) continue;
                items.push_back(MakeItem(name, CLspKind::Struct, entry.first, std::string(), start, col));
            }
            return items;
        }
    }

    const std::string prefix = word.found ? word.text : std::string();
    std::unordered_set<std::string> seen;
    // Nearest first: locals, then this file's own file-scope names and
    // the headers it includes, then macros, then keywords, then the
    // standard library. The server's order is the useful one, and
    // c_lsp_server.cpp pins it with sortText so a client cannot
    // alphabetize it away.
    for (const Symbol *sym : analyzer.VisibleSymbols()) {
        if (!MatchesPrefix(sym->name, prefix) || !seen.insert(sym->name).second) continue;
        std::string doc;
        if (!sym->path.empty()) doc = "declared in " + std::filesystem::path(sym->path).filename().string();
        items.push_back(MakeItem(sym->name, KindOfSymbol(*sym), sym->detail, doc, start, col));
    }
    for (const auto &entry : analyzer.macros()) {
        if (!MatchesPrefix(entry.first, prefix) || !seen.insert(entry.first).second) continue;
        items.push_back(MakeItem(entry.first, CLspKind::Constant, DescribeMacro(*entry.second), std::string(),
                                 start, col));
    }
    for (const CLspVocabEntry &entry : CLspKeywordVocab()) {
        if (!MatchesPrefix(entry.name, prefix) || !seen.insert(entry.name).second) continue;
        items.push_back(MakeItem(entry.name, CLspKind::Keyword, entry.detail, entry.doc, start, col));
    }
    // The standard library is large enough that offering all of it on an
    // empty prefix would be noise rather than help.
    if (!prefix.empty()) {
        for (size_t i = 0; i < kCStdDeclCount; i++) {
            const CStdDecl &decl = kCStdDecls[i];
            if (!MatchesPrefix(decl.name, prefix)) continue;
            if (!seen.insert(decl.name).second) continue;
            const CLspVocabEntry *vocab = CLspLookupStandardName(decl.name);
            items.push_back(MakeItem(decl.name,
                                     std::strchr(decl.detail, '(') != nullptr && std::strncmp(decl.detail, "#define", 7) != 0
                                         ? CLspKind::Function
                                         : CLspKind::Constant,
                                     decl.detail, vocab != nullptr && vocab->doc[0] != '\0'
                                                      ? std::string(vocab->doc) + "\n\n<" + decl.header + ">"
                                                      : "<" + std::string(decl.header) + ">",
                                     start, col));
            if (items.size() > 400) break;
        }
    }
    return items;
}

// --- Hover ------------------------------------------------------------

CLspHoverInfo CLspHover(const std::vector<std::string> &lines, int line, int col) {
    CLspHoverInfo info;
    CLspOptions opts;
    opts.check_files = false;
    Analyzer analyzer(lines, opts);
    analyzer.Collect();
    const CParseResult &parse = analyzer.parse();
    const std::string &text = LineAt(lines, line);

    // The header of an `#include` line, which is not an identifier.
    if (const CDirective *directive = DirectiveOnLine(parse, line);
        directive != nullptr && (directive->kind == CDirectiveKind::Include ||
                                 directive->kind == CDirectiveKind::Embed) &&
        !directive->header.empty() && col >= directive->header_col && col < directive->header_end_col) {
        info.found = true;
        info.line = line;
        info.col_start = directive->header_col;
        info.col_end = directive->header_end_col;
        const auto resolved = analyzer.resolved_includes().find(directive->header);
        if (resolved != analyzer.resolved_includes().end()) {
            info.text = directive->header + "\n\n" + resolved->second;
        } else if (CLspKnowsHeader(directive->header)) {
            const std::vector<CLspVocabEntry> *members = CLspHeaderMembers(directive->header);
            info.text = "<" + directive->header + ">\n\nA standard header. This server knows " +
                        std::to_string(members == nullptr ? 0 : members->size()) + " names from it.";
        } else {
            info.text = directive->header +
                        "\n\nThis server has no declarations for this header, so it stops reporting "
                        "undeclared names in this file.";
        }
        return info;
    }

    const WordAtCursor word = WordAt(lines, line, col, true);
    if (!word.found) return info;
    if (InStringOrComment(parse, line, col)) return info;
    info.line = line;
    info.col_start = word.start;
    info.col_end = word.end;

    // A member, whose meaning depends on what it is a member of.
    {
        int at = word.start;
        while (at > 0 && (text[static_cast<size_t>(at) - 1] == ' ' || text[static_cast<size_t>(at) - 1] == '\t')) at--;
        const bool arrow = at >= 2 && text.compare(static_cast<size_t>(at) - 2, 2, "->") == 0;
        const bool dot = at >= 1 && text[static_cast<size_t>(at) - 1] == '.' &&
                         !(at >= 2 && text[static_cast<size_t>(at) - 2] == '.');
        if (arrow || dot) {
            analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
            const CType type = ResolveChain(analyzer, ReadChainBefore(text, at - (arrow ? 3 : 2)));
            const CNode *rec = analyzer.RecordOf(type);
            const CNode *field = rec == nullptr ? nullptr : Analyzer::Field(rec, word.text);
            if (field != nullptr) {
                info.found = true;
                info.text = Analyzer::Describe(field) + "\n\nA member of " + analyzer.ResolveTypedef(type).base;
                return info;
            }
        }
    }

    if (IsCKeyword(word.text) || word.text == "bool" || word.text == "true" || word.text == "false") {
        for (const CLspVocabEntry &entry : CLspKeywordVocab()) {
            if (word.text != entry.name) continue;
            info.found = true;
            info.text = std::string(entry.name) + " -- " + entry.detail + "\n\n" + entry.doc;
            return info;
        }
    }

    if (const CDirective *macro = analyzer.FindMacro(word.text); macro != nullptr) {
        info.found = true;
        info.text = DescribeMacro(*macro);
        return info;
    }

    analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
    const Symbol *sym = analyzer.LookupConst(word.text);
    if (sym == nullptr) sym = analyzer.FindGlobal(word.text);
    if (sym != nullptr) {
        info.found = true;
        info.text = sym->detail;
        if (!sym->path.empty()) {
            info.text += "\n\nDeclared in " + std::filesystem::path(sym->path).filename().string();
        } else if (sym->pos.line != line) {
            info.text += "\n\nDeclared on line " + std::to_string(sym->pos.line + 1);
        }
        return info;
    }

    if (const CLspVocabEntry *entry = CLspLookupStandardName(word.text); entry != nullptr) {
        info.found = true;
        info.text = entry->detail;
        const std::string header = CLspHeaderOf(word.text);
        if (!header.empty()) info.text += "\n\n<" + header + ">";
        if (entry->doc[0] != '\0') info.text += "\n\n" + std::string(entry->doc);
        return info;
    }
    return info;
}

// --- Document symbols -------------------------------------------------

namespace {

// LSP SymbolKind, only the ones this server emits.
constexpr int kSymbolModule = 2;
constexpr int kSymbolField = 8;
constexpr int kSymbolEnum = 10;
constexpr int kSymbolFunction = 12;
constexpr int kSymbolVariable = 13;
constexpr int kSymbolConstant = 14;
constexpr int kSymbolEnumMember = 22;
constexpr int kSymbolStruct = 23;
constexpr int kSymbolTypeParameter = 26;

/** @brief Appends one symbol, returning its index so children can point at it. */
int AddSymbol(std::vector<CLspSymbol> *out, const std::string &name, const std::string &detail, int kind,
              const CNode *node, int parent) {
    CLspSymbol sym;
    sym.name = name;
    sym.detail = detail;
    sym.kind = kind;
    sym.line_start = node->start.line;
    sym.line_end = node->end.line;
    sym.sel_line = node->name_pos.line;
    sym.sel_col_start = node->name_pos.col;
    sym.sel_col_end = node->name_end_col > node->name_pos.col ? node->name_end_col
                                                              : node->name_pos.col + static_cast<int>(name.size());
    sym.parent = parent;
    out->push_back(sym);
    return static_cast<int>(out->size()) - 1;
}

/** @brief Appends a struct, union or enum definition and its members. */
void AddTagSymbols(std::vector<CLspSymbol> *out, const CNode *tag, int parent, const std::string &typedef_name) {
    const bool is_enum = tag->kind == CNodeKind::EnumDecl;
    std::string name = tag->name;
    if (name.empty()) name = typedef_name.empty() ? std::string("(anonymous)") : typedef_name;
    const std::string detail = is_enum ? "enum" : tag->str_value;
    const int index = AddSymbol(out, name, detail, is_enum ? kSymbolEnum : kSymbolStruct, tag, parent);
    for (const CNodePtr &member : tag->body) {
        if (member->kind == CNodeKind::Enumerator && !member->name.empty()) {
            AddSymbol(out, member->name, "enumeration constant", kSymbolEnumMember, member.get(), index);
        } else if (member->kind == CNodeKind::Field && !member->name.empty()) {
            AddSymbol(out, member->name, member->type.spelling, kSymbolField, member.get(), index);
        } else if (member->kind == CNodeKind::RecordDecl || member->kind == CNodeKind::EnumDecl) {
            AddTagSymbols(out, member.get(), index, std::string());
        }
    }
}

}  // namespace

std::vector<CLspSymbol> CLspSymbols(const std::vector<std::string> &lines) {
    std::vector<CLspSymbol> out;
    const CParseResult parse = ParseC(lines);

    // Macros first, in document order among themselves; a header's
    // outline is mostly macros, and hiding them would make the outline
    // useless for exactly the files that need it most.
    for (const CDirective &d : parse.directives) {
        if (d.kind != CDirectiveKind::Define || d.name.empty()) continue;
        CLspSymbol sym;
        sym.name = d.name;
        sym.detail = DescribeMacro(d);
        sym.kind = d.function_like ? kSymbolModule : kSymbolConstant;
        sym.line_start = d.line;
        sym.line_end = d.end_line;
        sym.sel_line = d.name_pos.line;
        sym.sel_col_start = d.name_pos.col;
        sym.sel_col_end = d.name_end_col;
        out.push_back(sym);
    }

    for (const CNodePtr &node : parse.unit->body) {
        if (node->kind == CNodeKind::FunctionDef) {
            if (node->name.empty()) continue;
            AddSymbol(&out, node->name, Analyzer::Describe(node.get()), kSymbolFunction, node.get(), -1);
            continue;
        }
        if (node->kind != CNodeKind::Declaration) continue;
        // `typedef struct { ... } Name;` should outline under `Name`,
        // which is the only name anyone reading the file will look for.
        std::string typedef_name;
        if ((node->flags & kCFlagTypedef) != 0 && !node->kids.empty()) {
            for (const CNodePtr &d : node->kids) {
                if (d->kind == CNodeKind::Declarator && !d->name.empty()) {
                    typedef_name = d->name;
                    break;
                }
            }
        }
        for (const CNodePtr &kid : node->kids) {
            if (kid->kind == CNodeKind::RecordDecl || kid->kind == CNodeKind::EnumDecl) {
                if (!kid->body.empty() || !kid->name.empty()) AddTagSymbols(&out, kid.get(), -1, typedef_name);
            }
        }
        for (const CNodePtr &kid : node->kids) {
            if (kid->kind != CNodeKind::Declarator || kid->name.empty()) continue;
            int kind = kSymbolVariable;
            if ((node->flags & kCFlagTypedef) != 0) {
                kind = kSymbolTypeParameter;
                // A typedef of an anonymous struct already has an entry
                // under its own name; a second one would be a duplicate.
                bool anonymous_tag = false;
                for (const CNodePtr &tag : node->kids) {
                    if ((tag->kind == CNodeKind::RecordDecl || tag->kind == CNodeKind::EnumDecl) &&
                        tag->name.empty() && !tag->body.empty()) {
                        anonymous_tag = true;
                    }
                }
                if (anonymous_tag && kid->name == typedef_name) continue;
            } else if (kid->type.is_function) {
                kind = kSymbolFunction;
            } else if (kid->type.is_const || (node->flags & kCFlagConst) != 0) {
                kind = kSymbolConstant;
            }
            AddSymbol(&out, kid->name, Analyzer::Describe(kid.get()), kind, kid.get(), -1);
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const CLspSymbol &a, const CLspSymbol &b) {
        // Parent links are indices into this vector, so only the
        // top-level entries may move; children stay next to their
        // parent because they sort by the same line.
        return a.line_start < b.line_start;
    });
    // Sorting invalidated the parent indices, so rebuild them by
    // identity: a child's parent is the nearest earlier symbol whose
    // range encloses it.
    for (size_t i = 0; i < out.size(); i++) {
        out[i].parent = -1;
        for (size_t j = i; j > 0; j--) {
            const CLspSymbol &candidate = out[j - 1];
            const bool encloses = candidate.line_start <= out[i].line_start && out[i].line_end <= candidate.line_end;
            if (!encloses) continue;
            if (candidate.kind != kSymbolStruct && candidate.kind != kSymbolEnum) continue;
            out[i].parent = static_cast<int>(j - 1);
            break;
        }
    }
    return out;
}

// --- Folding ----------------------------------------------------------

std::vector<CLspFold> CLspFolds(const std::vector<std::string> &lines) {
    std::vector<CLspFold> folds;
    const CParseResult parse = ParseC(lines);

    // Braced regions, from the token stream rather than the tree: a file
    // being typed into still has balanced braces above the cursor, and
    // the tree may not.
    std::vector<CPos> open_braces;
    for (const CToken &t : parse.tokens) {
        if (t.kind != CTokKind::Punct || t.directive >= 0) continue;
        if (t.text == "{") {
            open_braces.push_back(t.start);
            continue;
        }
        if (t.text != "}" || open_braces.empty()) continue;
        const CPos open = open_braces.back();
        open_braces.pop_back();
        if (t.start.line > open.line) {
            CLspFold fold;
            fold.start_line = open.line;
            fold.end_line = t.start.line - 1;
            folds.push_back(fold);
        }
    }

    // Conditional groups: `#if` to its `#endif`.
    for (size_t i = 0; i < parse.directives.size(); i++) {
        const CDirective &d = parse.directives[i];
        const bool opens = d.kind == CDirectiveKind::If || d.kind == CDirectiveKind::Ifdef ||
                           d.kind == CDirectiveKind::Ifndef;
        if (!opens || d.matching_end < 0) continue;
        const CDirective &end = parse.directives[static_cast<size_t>(d.matching_end)];
        if (end.line <= d.end_line) continue;
        CLspFold fold;
        fold.start_line = d.line;
        fold.end_line = end.line - 1;
        folds.push_back(fold);
    }

    // The run of `#include` lines at the top, which is what a reader
    // folds away first.
    int include_start = -1;
    int include_end = -1;
    for (const CDirective &d : parse.directives) {
        if (d.kind != CDirectiveKind::Include) continue;
        if (include_start < 0) {
            include_start = d.line;
            include_end = d.end_line;
            continue;
        }
        // A blank line or a comment between two includes keeps the run
        // going; anything else ends it.
        bool contiguous = true;
        for (int line = include_end + 1; line < d.line; line++) {
            const std::string &text = LineAt(lines, line);
            const std::string trimmed = Trim(text);
            if (trimmed.empty() || trimmed.rfind("//", 0) == 0 || trimmed.rfind("/*", 0) == 0 ||
                trimmed.rfind("*", 0) == 0 || trimmed[0] == '#') {
                continue;
            }
            contiguous = false;
            break;
        }
        if (!contiguous) break;
        include_end = d.end_line;
    }
    if (include_start >= 0 && include_end > include_start) {
        CLspFold fold;
        fold.start_line = include_start;
        fold.end_line = include_end;
        fold.kind = "imports";
        folds.push_back(fold);
    }

    // Comment runs: a block comment spanning lines, and a run of `//`
    // lines that are alone on their lines.
    for (size_t i = 0; i < parse.comments.size(); i++) {
        const CComment &c = parse.comments[i];
        if (c.block) {
            if (c.end_line > c.line) {
                CLspFold fold;
                fold.start_line = c.line;
                fold.end_line = c.end_line;
                fold.kind = "comment";
                folds.push_back(fold);
            }
            continue;
        }
        if (!c.own_line) continue;
        size_t j = i;
        while (j + 1 < parse.comments.size() && !parse.comments[j + 1].block && parse.comments[j + 1].own_line &&
               parse.comments[j + 1].line == parse.comments[j].line + 1) {
            j++;
        }
        if (j > i) {
            CLspFold fold;
            fold.start_line = c.line;
            fold.end_line = parse.comments[j].line;
            fold.kind = "comment";
            folds.push_back(fold);
        }
        i = j;
    }

    std::stable_sort(folds.begin(), folds.end(),
                     [](const CLspFold &a, const CLspFold &b) { return a.start_line < b.start_line; });
    return folds;
}

// --- Definition -------------------------------------------------------

CLspLocation CLspDefinition(const std::vector<std::string> &lines, int line, int col, const CLspOptions &opts) {
    CLspLocation location;
    Analyzer analyzer(lines, opts);
    analyzer.Collect();
    const CParseResult &parse = analyzer.parse();
    const std::string &text = LineAt(lines, line);

    // An `#include` jumps to the file it names.
    if (const CDirective *directive = DirectiveOnLine(parse, line);
        directive != nullptr && directive->kind == CDirectiveKind::Include && !directive->header.empty() &&
        col >= directive->header_col && col < directive->header_end_col) {
        const auto resolved = analyzer.resolved_includes().find(directive->header);
        if (resolved == analyzer.resolved_includes().end()) return location;
        location.found = true;
        location.path = resolved->second;
        return location;
    }

    const WordAtCursor word = WordAt(lines, line, col, true);
    if (!word.found || InStringOrComment(parse, line, col)) return location;

    // A member resolves to the field's own declaration.
    {
        int at = word.start;
        while (at > 0 && (text[static_cast<size_t>(at) - 1] == ' ' || text[static_cast<size_t>(at) - 1] == '\t')) at--;
        const bool arrow = at >= 2 && text.compare(static_cast<size_t>(at) - 2, 2, "->") == 0;
        const bool dot = at >= 1 && text[static_cast<size_t>(at) - 1] == '.' &&
                         !(at >= 2 && text[static_cast<size_t>(at) - 2] == '.');
        if (arrow || dot) {
            analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
            const CType type = ResolveChain(analyzer, ReadChainBefore(text, at - (arrow ? 3 : 2)));
            const CNode *rec = analyzer.RecordOf(type);
            const CNode *field = rec == nullptr ? nullptr : Analyzer::Field(rec, word.text);
            if (field != nullptr) {
                location.found = true;
                location.line = field->name_pos.line;
                location.col = field->name_pos.col;
                return location;
            }
        }
    }

    if (const CDirective *macro = analyzer.FindMacro(word.text); macro != nullptr) {
        location.found = true;
        location.line = macro->name_pos.line;
        location.col = macro->name_pos.col;
        return location;
    }

    analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
    const Symbol *sym = analyzer.LookupConst(word.text);
    if (sym == nullptr) sym = analyzer.FindGlobal(word.text);
    if (sym == nullptr) return location;
    location.found = true;
    location.path = sym->path;
    location.line = sym->pos.line;
    location.col = sym->pos.col;
    return location;
}

// --- References, highlight and rename ---------------------------------

CLspReferenceSet CLspReferences(const std::vector<std::string> &lines, int line, int col) {
    CLspReferenceSet refs;
    CLspOptions opts;
    opts.check_files = false;
    Analyzer analyzer(lines, opts);
    analyzer.Collect();
    const CParseResult &parse = analyzer.parse();
    const WordAtCursor word = WordAt(lines, line, col, true);
    if (!word.found || InStringOrComment(parse, line, col)) return refs;
    refs.found = true;
    refs.name = word.text;

    if (IsCKeyword(word.text)) {
        refs.rename_blocked_reason = "`" + word.text + "` is a keyword";
    }

    // The range the binding lives in. A local is renamed only inside its
    // own function; anything at file scope, and every macro, is renamed
    // across the whole document. That is the honest limit of a
    // single-file server: it will not follow a global into another
    // translation unit, so it says so rather than half-doing it.
    int from_line = 0;
    int to_line = static_cast<int>(lines.size());
    analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
    const Symbol *sym = analyzer.LookupConst(word.text);
    const bool is_local = sym != nullptr && sym->scope > 0;
    if (is_local) {
        if (const CNode *fn = analyzer.EnclosingFunction(line); fn != nullptr) {
            from_line = fn->start.line;
            to_line = fn->end.line + 1;
        }
    }
    if (sym == nullptr) {
        const Symbol *global = analyzer.FindGlobal(word.text);
        if (global != nullptr && !global->path.empty()) {
            refs.rename_blocked_reason = "`" + word.text + "` is declared in another file";
        } else if (global == nullptr && analyzer.FindMacro(word.text) == nullptr &&
                   refs.rename_blocked_reason.empty() && IsStandardName(word.text)) {
            refs.rename_blocked_reason = "`" + word.text + "` comes from the standard library";
        }
    }

    for (const CToken &t : parse.tokens) {
        if (t.kind != CTokKind::Ident || t.text != word.text) continue;
        if (t.start.line < from_line || t.start.line >= to_line) continue;
        CLspReference ref;
        ref.line = t.start.line;
        ref.col_start = t.start.col;
        ref.col_end = t.end.col;
        refs.refs.push_back(ref);
    }
    // The declaration, and anything assigned to, is a write. Worked out
    // from the token stream rather than the tree so that a half-typed
    // file still highlights.
    for (CLspReference &ref : refs.refs) {
        if (sym != nullptr && ref.line == sym->pos.line && ref.col_start == sym->pos.col) {
            ref.is_write = true;
            continue;
        }
        const std::string &text = LineAt(lines, ref.line);
        size_t after = static_cast<size_t>(ref.col_end);
        while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) after++;
        if (after < text.size() && text[after] == '=' && (after + 1 >= text.size() || text[after + 1] != '=')) {
            ref.is_write = true;
        }
    }
    std::stable_sort(refs.refs.begin(), refs.refs.end(), [](const CLspReference &a, const CLspReference &b) {
        return a.line != b.line ? a.line < b.line : a.col_start < b.col_start;
    });
    return refs;
}

// --- Signature help ---------------------------------------------------

CLspSignature CLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    CLspSignature signature;
    CLspOptions opts;
    opts.check_files = false;
    Analyzer analyzer(lines, opts);
    analyzer.Collect();

    // Scan backwards for the innermost `(` that is still open, counting
    // the commas at its own level on the way. Lexical on purpose, for
    // the same reason member completion is: this runs mid-call, when the
    // call is not yet a call.
    int depth = 0;
    int active = 0;
    int open_line = -1;
    int open_col = -1;
    for (int l = line; l >= 0 && open_line < 0; l--) {
        const std::string &text = LineAt(lines, l);
        int start = l == line ? std::min(col, static_cast<int>(text.size())) : static_cast<int>(text.size());
        for (int i = start - 1; i >= 0; i--) {
            const char c = text[static_cast<size_t>(i)];
            if (c == ')' || c == ']' || c == '}') {
                depth++;
                continue;
            }
            if (c == '[' || c == '{') {
                if (depth == 0) return signature;  // not inside a call at all
                depth--;
                continue;
            }
            if (c == '(') {
                if (depth > 0) {
                    depth--;
                    continue;
                }
                open_line = l;
                open_col = i;
                break;
            }
            if (c == ',' && depth == 0) active++;
            if (c == ';' && depth == 0) return signature;
        }
        if (line - l > 40) break;  // a call spanning forty lines is not one this helps with
    }
    if (open_line < 0) return signature;

    const std::string &open_text = LineAt(lines, open_line);
    int name_end = open_col;
    while (name_end > 0 && (open_text[static_cast<size_t>(name_end) - 1] == ' ' ||
                            open_text[static_cast<size_t>(name_end) - 1] == '\t')) {
        name_end--;
    }
    int name_start = name_end;
    while (name_start > 0 && IsWordChar(open_text[static_cast<size_t>(name_start) - 1])) name_start--;
    if (name_start == name_end) return signature;
    const std::string name =
        open_text.substr(static_cast<size_t>(name_start), static_cast<size_t>(name_end - name_start));
    if (IsCKeyword(name)) return signature;

    signature.active_param = active;

    // A function this file or one of its headers declares: the
    // parameters are real nodes, so each one's own label is exact.
    analyzer.PushLocalsAt(analyzer.EnclosingFunction(line), line, col);
    const Symbol *sym = analyzer.LookupConst(name);
    if (sym == nullptr) sym = analyzer.FindGlobal(name);
    if (sym != nullptr && sym->node != nullptr && !sym->node->params.empty()) {
        signature.found = true;
        signature.label = sym->detail;
        for (const CNodePtr &param : sym->node->params) {
            signature.params.push_back(Analyzer::DescribeParam(param.get()));
        }
        return signature;
    }
    if (const CDirective *macro = analyzer.FindMacro(name); macro != nullptr && macro->function_like) {
        signature.found = true;
        signature.label = DescribeMacro(*macro);
        signature.params = macro->params;
        signature.documentation = "A function-like macro, so its arguments are text, not values.";
        return signature;
    }
    // Otherwise, the standard library's own declaration, whose parameter
    // list is split back out of the generated signature text.
    const CLspVocabEntry *entry = CLspLookupStandardName(name);
    if (entry == nullptr) return signature;
    const std::string detail = entry->detail;
    const size_t open_paren = detail.find('(');
    if (open_paren == std::string::npos || detail.rfind("#define", 0) == 0) {
        if (detail.rfind("#define", 0) != 0) return signature;
    }
    signature.found = true;
    signature.label = detail;
    if (entry->doc[0] != '\0') signature.documentation = entry->doc;
    const std::string header = CLspHeaderOf(name);
    if (!header.empty()) {
        signature.documentation += signature.documentation.empty() ? "" : "\n\n";
        signature.documentation += "<" + header + ">";
    }
    if (open_paren != std::string::npos) {
        const size_t close = detail.rfind(')');
        if (close != std::string::npos && close > open_paren) {
            const std::string params = detail.substr(open_paren + 1, close - open_paren - 1);
            int nesting = 0;
            std::string current;
            for (char c : params) {
                if (c == '(' || c == '[') nesting++;
                if (c == ')' || c == ']') nesting--;
                if (c == ',' && nesting == 0) {
                    signature.params.push_back(Trim(current));
                    current.clear();
                    continue;
                }
                current += c;
            }
            if (!Trim(current).empty()) signature.params.push_back(Trim(current));
        }
    }
    return signature;
}
