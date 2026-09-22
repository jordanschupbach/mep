// The scope model of mep's own C++ language server: one walk of the
// syntax tree that produces every declaration, the scopes they live in
// and every place a name is used. cpp_lsp_index.h documents the shapes;
// this file builds them, and cpp_lsp_diagnostics.cpp and
// cpp_lsp_features.cpp are the two consumers.
//
// The one place this reaches outside the document is
// LoadIncludedHeaders: a quoted `#include` that resolves to a real file
// next to the document is parsed too, and its top-level declarations are
// added as external symbols. That is what makes go to definition land in
// a project's own header, and it is also what makes the undefined-name
// check possible at all -- without it, every name from every header
// would be unknown and the check would have to stay switched off (see
// cpp_lsp.h). Headers are cached by path and modification time, so
// re-analysing a document after a keystroke re-reads nothing.

#include "cpp_lsp.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpp_ast.h"
#include "cpp_lsp_index.h"

namespace {

// A header's declarations cost one parse each; the budget keeps a
// deeply-including translation unit from making every keystroke slow.
constexpr int kMaxHeaderDepth = 4;
constexpr size_t kMaxHeaderFiles = 60;

std::string Trim(const std::string &s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) end--;
    return s.substr(start, end - start);
}

bool EndsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** @brief Reads a file into lines, returning false when it cannot be opened. */
bool ReadFileLines(const std::string &path, std::vector<std::string> *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out->push_back(line);
    }
    if (out->empty()) out->push_back("");
    return true;
}

// --- The header cache -------------------------------------------------

// What a header contributes to the document that included it: its
// top-level names, where they are, and how to describe them. The full
// syntax tree is deliberately not kept -- it is the largest part of a
// parse and nothing outside the header's own file needs it.
struct HeaderSymbol {
    std::string name;
    std::string qualified;
    CppSymbolKind kind = CppSymbolKind::Variable;
    int line = 0;
    int col = 0;
    int end_col = 0;
    std::string detail;
    std::string doc;
    std::string type_base;
    bool is_function_like_macro = false;
};

struct CachedHeader {
    std::filesystem::file_time_type mtime{};
    std::vector<HeaderSymbol> symbols;
    std::vector<std::string> includes;  // quoted includes, already resolved to paths
    bool valid = false;
};

std::map<std::string, CachedHeader> &HeaderCache() {
    static std::map<std::string, CachedHeader> cache;
    return cache;
}

}  // namespace

namespace {

// --- Small helpers ----------------------------------------------------

CppSymbolKind KindForClass(const CppNode *node) {
    if (node->str_value == "struct") return CppSymbolKind::Struct;
    if (node->str_value == "union") return CppSymbolKind::Union;
    return CppSymbolKind::Class;
}

/** @brief The bare name inside a type's spelling: `const std::vector<int> &` -> `std::vector`. */
std::string TypeBaseOf(const CppNode *type) {
    if (type == nullptr) return "";
    if (!type->type_base.empty()) return type->type_base;
    std::string text = type->type_text;
    // Strip qualifiers and declarator punctuation, keep the last name.
    static const char *const kDrop[] = {"const", "volatile", "static", "inline", "constexpr",
                                        "mutable", "struct", "class", "enum", "typename", "unsigned",
                                        "signed"};
    std::string out;
    std::string word;
    for (size_t i = 0; i <= text.size(); i++) {
        const char c = i < text.size() ? text[i] : ' ';
        if (IsCppIdentChar(static_cast<unsigned char>(c)) || c == ':') {
            word += c;
            continue;
        }
        if (!word.empty()) {
            bool drop = false;
            for (const char *skip : kDrop) {
                if (word == skip) drop = true;
            }
            if (!drop) out = word;
            word.clear();
        }
        if (c == '<') break;  // template arguments are not part of the base name
    }
    return out;
}

/** @brief Reports whether a written type is made only of fundamental type keywords (`unsigned char`). */
bool IsFundamentalSpelling(const std::string &text) {
    if (text.empty()) return false;
    std::string word;
    bool any = false;
    for (size_t i = 0; i <= text.size(); i++) {
        const char c = i < text.size() ? text[i] : ' ';
        if (IsCppIdentChar(static_cast<unsigned char>(c))) {
            word += c;
            continue;
        }
        if (!word.empty()) {
            if (!IsCppFundamentalType(word) && word != "const" && word != "volatile") return false;
            any = true;
            word.clear();
        }
    }
    return any;
}

/** @brief Renders a parameter list the way hover and signature help show it. */
std::string ParameterListText(const CppNode *fn) {
    std::string out = "(";
    for (size_t i = 0; i < fn->params.size(); i++) {
        const CppNode *param = fn->params[i].get();
        if (param == nullptr) continue;
        if (i > 0) out += ", ";
        if (param->is_variadic && param->name.empty() && param->kids.empty()) {
            out += "...";
            continue;
        }
        const std::string type = param->kids.empty() ? "" : CppTypeText(param->kids.front().get());
        out += type;
        if (!param->name.empty()) {
            if (!type.empty() && type.back() != '*' && type.back() != '&' && type.back() != ' ') out += ' ';
            out += param->name;
        }
        if (param->is_variadic) out += "...";
    }
    out += ")";
    return out;
}

}  // namespace

std::string CppSymbolDetail(const CppSymbol &symbol) {
    if (!symbol.detail.empty()) return symbol.detail;
    if (!symbol.type_text.empty()) return symbol.type_text + " " + symbol.name;
    return symbol.name;
}

namespace {

// --- The index builder ------------------------------------------------

class IndexBuilder {
public:
    IndexBuilder(CppIndex &index, const CppLspOptions &opts) : index_(index), opts_(opts) {}

    void Build() {
        CppScope file_scope;
        file_scope.kind = CppScope::Kind::File;
        file_scope.parent = -1;
        index_.scopes.push_back(file_scope);

        AddMacroSymbols();
        LoadIncludedHeaders();
        if (index_.parse.unit) WalkDeclarations(index_.parse.unit->body, 0);
        ResolvePendingBases();
        ResolveForwardReferences();
    }

private:
    CppIndex &index_;
    const CppLspOptions &opts_;
    // Class scopes whose base clause named something, resolved after the
    // whole file is walked so a base declared further down still counts.
    std::vector<std::pair<int, std::vector<std::string>>> pending_bases_;
    // Scopes created for classes read out of headers, by qualified name.
    std::map<std::string, int> external_scopes_;
    // True while walking the target of a read-modify-write.
    bool read_write_ = false;

    // --- Symbols and scopes -------------------------------------------

    int NewScope(CppScope::Kind kind, int parent, const std::string &name, const CppNode *node) {
        CppScope scope;
        scope.kind = kind;
        scope.parent = parent;
        scope.name = name;
        scope.node = node;
        if (node != nullptr) {
            scope.start = node->start;
            scope.end = node->end;
        }
        const std::string &parent_qualified = index_.scopes[static_cast<size_t>(parent)].qualified;
        if (!name.empty()) {
            scope.qualified = parent_qualified.empty() ? name : parent_qualified + "::" + name;
        } else {
            scope.qualified = parent_qualified;
        }
        index_.scopes.push_back(scope);
        const int created = static_cast<int>(index_.scopes.size()) - 1;
        index_.scopes[static_cast<size_t>(parent)].children.push_back(created);
        return created;
    }

    int AddSymbol(CppSymbol symbol) {
        const int scope = symbol.scope;
        const std::string &qualifier = index_.scopes[static_cast<size_t>(scope)].qualified;
        if (symbol.qualified.empty()) {
            symbol.qualified = qualifier.empty() ? symbol.name : qualifier + "::" + symbol.name;
        }
        index_.symbols.push_back(std::move(symbol));
        const int created = static_cast<int>(index_.symbols.size()) - 1;
        index_.scopes[static_cast<size_t>(scope)].symbols.push_back(created);
        return created;
    }

    void RecordOccurrence(const std::string &name, const CppPos &pos, const CppPos &end, int symbol, bool is_write,
                          bool is_declaration, bool is_member, int owner_type = -1, int scope = 0,
                          bool also_reads = false) {
        if (name.empty()) return;
        CppOccurrence occurrence;
        occurrence.pos = pos;
        occurrence.length = end.line == pos.line ? std::max(0, end.col - pos.col) : static_cast<int>(name.size());
        if (occurrence.length == 0) occurrence.length = static_cast<int>(name.size());
        occurrence.symbol = symbol;
        occurrence.is_write = is_write;
        occurrence.is_declaration = is_declaration;
        occurrence.is_member = is_member;
        occurrence.owner_type = owner_type;
        occurrence.scope = scope;
        occurrence.name = name;
        index_.occurrences.push_back(occurrence);
        if (symbol >= 0 && !is_declaration) {
            CppSymbol &target = index_.symbols[static_cast<size_t>(symbol)];
            if (is_write) target.writes++;
            // A read-modify-write reads too: `guard++ < 200` and `n += 1`
            // both use the old value, and counting them as writes alone
            // is what makes a loop counter look unread.
            if (!is_write || also_reads) target.reads++;
        }
    }

    // --- Macros and headers -------------------------------------------

    void AddMacroSymbols() {
        for (const auto &entry : index_.parse.macros) {
            const CppMacro &macro = entry.second;
            CppSymbol symbol;
            symbol.name = macro.name;
            symbol.kind = CppSymbolKind::Macro;
            symbol.pos.line = macro.line;
            symbol.pos.col = MacroNameColumn(macro);
            symbol.end = symbol.pos;
            symbol.end.col += static_cast<int>(macro.name.size());
            symbol.range_start = symbol.pos;
            symbol.range_end = symbol.end;
            symbol.is_function_like_macro = macro.function_like;
            symbol.scope = 0;
            std::string detail = "#define " + macro.name;
            if (macro.function_like) {
                detail += "(";
                for (size_t i = 0; i < macro.params.size(); i++) {
                    if (i > 0) detail += ", ";
                    detail += macro.params[i];
                }
                detail += ")";
            }
            std::string body;
            for (const CppToken &token : macro.body) {
                if (!body.empty() && token.space_before) body += ' ';
                body += token.text;
                if (body.size() > 120) {
                    body += " ...";
                    break;
                }
            }
            if (!body.empty()) detail += " " + body;
            symbol.detail = detail;
            AddSymbol(std::move(symbol));
        }
    }

    /** @brief Finds the column of a macro's name on its `#define` line. */
    int MacroNameColumn(const CppMacro &macro) const {
        if (macro.line < 0 || static_cast<size_t>(macro.line) >= index_.lines.size()) return 0;
        const std::string &line = index_.lines[static_cast<size_t>(macro.line)];
        const size_t at = line.find(macro.name);
        return at == std::string::npos ? 0 : static_cast<int>(at);
    }

    void LoadIncludedHeaders() {
        if (!opts_.check_files) {
            for (const CppInclude &include : index_.parse.includes) {
                if (include.resolved.empty()) index_.has_unresolved_include = true;
            }
            return;
        }
        std::set<std::string> visited;
        std::vector<std::pair<std::string, int>> queue;
        for (const CppInclude &include : index_.parse.includes) {
            if (!include.resolved.empty()) {
                queue.emplace_back(include.resolved, 1);
                continue;
            }
            // A standard header this server has a vocabulary for is
            // "seen"; anything else is a hole in what it can know, and
            // the name checks step aside because of it.
            if (!(include.angled && IsKnownStandardHeader(include.header))) {
                index_.has_unresolved_include = true;
            }
        }
        while (!queue.empty() && visited.size() < kMaxHeaderFiles) {
            const std::pair<std::string, int> item = queue.back();
            queue.pop_back();
            if (item.second > kMaxHeaderDepth) continue;
            if (!visited.insert(item.first).second) continue;
            const CachedHeader *header = LoadHeader(item.first);
            if (header == nullptr) {
                index_.has_unresolved_include = true;
                continue;
            }
            for (const HeaderSymbol &entry : header->symbols) AddExternalSymbol(entry, item.first);
            for (const std::string &next : header->includes) queue.emplace_back(next, item.second + 1);
        }
    }

    static bool IsKnownStandardHeader(const std::string &header) {
        for (const CppLspVocabEntry &entry : CppLspHeaderVocab()) {
            if (header == entry.name) return true;
        }
        return false;
    }

    void AddExternalSymbol(const HeaderSymbol &entry, const std::string &path) {
        CppSymbol symbol;
        symbol.name = entry.name;
        symbol.qualified = entry.qualified;
        symbol.kind = entry.kind;
        symbol.pos.line = entry.line;
        symbol.pos.col = entry.col;
        symbol.end = symbol.pos;
        symbol.end.col = entry.end_col;
        symbol.range_start = symbol.pos;
        symbol.range_end = symbol.end;
        symbol.detail = entry.detail;
        symbol.doc = entry.doc;
        symbol.type_base = entry.type_base;
        symbol.is_function_like_macro = entry.is_function_like_macro;
        symbol.external = true;
        symbol.file = path;
        // A class from a header gets a real scope, with its members in
        // it: that is what makes `editor.DrawPane()` resolve in a .cpp
        // file whose class lives in the .h next to it. The qualifier of
        // each name says where it belongs, so the hierarchy is rebuilt
        // from the qualified names rather than re-walked.
        const size_t at = entry.qualified.rfind("::");
        const std::string qualifier = at == std::string::npos ? "" : entry.qualified.substr(0, at);
        const auto owner = external_scopes_.find(qualifier);
        symbol.scope = owner == external_scopes_.end() ? 0 : owner->second;
        const bool opens_scope = entry.kind == CppSymbolKind::Class || entry.kind == CppSymbolKind::Struct ||
                                 entry.kind == CppSymbolKind::Union || entry.kind == CppSymbolKind::Enum;
        const int parent_scope = symbol.scope;
        // A header that forward-declares a class and then defines it
        // yields two entries for the same name; they share one scope, or
        // a lookup finds the empty one and the class looks memberless.
        const auto existing = opens_scope ? external_scopes_.find(entry.qualified) : external_scopes_.end();
        if (existing != external_scopes_.end()) symbol.child_scope = existing->second;
        const int created = AddSymbol(std::move(symbol));
        if (!opens_scope || existing != external_scopes_.end()) return;
        const int class_scope = NewScope(CppScope::Kind::Class, parent_scope, entry.name, nullptr);
        index_.scopes[static_cast<size_t>(class_scope)].owner = created;
        index_.symbols[static_cast<size_t>(created)].child_scope = class_scope;
        external_scopes_[entry.qualified] = class_scope;
    }

    /** @brief Parses a header (or returns the cached result) and extracts its top-level declarations. */
    const CachedHeader *LoadHeader(const std::string &path) {
        std::error_code ec;
        const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, ec);
        if (ec) return nullptr;
        auto &cache = HeaderCache();
        const auto found = cache.find(path);
        if (found != cache.end() && found->second.mtime == mtime) {
            return found->second.valid ? &found->second : nullptr;
        }
        CachedHeader entry;
        entry.mtime = mtime;
        std::vector<std::string> lines;
        if (!ReadFileLines(path, &lines)) {
            cache[path] = entry;
            return nullptr;
        }
        CppParseOptions parse_opts;
        parse_opts.doc_dir = std::filesystem::path(path).parent_path().string();
        parse_opts.include_dirs = opts_.include_dirs;
        parse_opts.resolve_includes = true;
        parse_opts.defines = opts_.defines;
        const CppParseResult parsed = ParseCpp(lines, parse_opts);
        CollectHeaderSymbols(parsed, lines, "", parsed.unit.get(), &entry.symbols);
        for (const auto &macro : parsed.macros) {
            HeaderSymbol symbol;
            symbol.name = macro.first;
            symbol.qualified = macro.first;
            symbol.kind = CppSymbolKind::Macro;
            symbol.line = macro.second.line;
            symbol.col = 0;
            symbol.end_col = static_cast<int>(macro.first.size());
            symbol.is_function_like_macro = macro.second.function_like;
            symbol.detail = "#define " + macro.first;
            entry.symbols.push_back(symbol);
        }
        for (const CppInclude &include : parsed.includes) {
            if (!include.resolved.empty()) entry.includes.push_back(include.resolved);
        }
        entry.valid = true;
        cache[path] = std::move(entry);
        return &cache[path];
    }

    /** @brief Walks a parsed header's declarations, recording the names it makes visible. */
    static void CollectHeaderSymbols(const CppParseResult &parsed, const std::vector<std::string> &lines,
                                     const std::string &qualifier, const CppNode *node,
                                     std::vector<HeaderSymbol> *out) {
        if (node == nullptr || out->size() > 4000) return;
        for (const CppNodePtr &child : node->body) {
            const CppNode *decl = child.get();
            if (decl == nullptr) continue;
            switch (decl->kind) {
                case CppNodeKind::Namespace: {
                    const std::string inner =
                        decl->name.empty() ? qualifier : (qualifier.empty() ? decl->name : qualifier + "::" + decl->name);
                    CollectHeaderSymbols(parsed, lines, inner, decl, out);
                    break;
                }
                case CppNodeKind::LinkageSpec:
                    CollectHeaderSymbols(parsed, lines, qualifier, decl, out);
                    break;
                case CppNodeKind::Class:
                case CppNodeKind::Enum: {
                    if (decl->name.empty()) break;
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = decl->kind == CppNodeKind::Enum ? CppSymbolKind::Enum : KindForClass(decl);
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    symbol.detail = decl->str_value + " " + symbol.qualified;
                    out->push_back(symbol);
                    // A header's class members matter too: `Editor::Draw`
                    // is what a .cpp file defines, and its fields are
                    // what a completion after `editor.` should offer.
                    const std::string inner = symbol.qualified;
                    CollectHeaderSymbols(parsed, lines, inner, decl, out);
                    break;
                }
                case CppNodeKind::Enumerator: {
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = CppSymbolKind::Enumerator;
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    out->push_back(symbol);
                    break;
                }
                case CppNodeKind::Function: {
                    if (decl->name.empty()) break;
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = qualifier.empty() ? CppSymbolKind::Function : CppSymbolKind::Method;
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    const std::string return_type =
                        decl->kids.empty() ? "" : CppTypeText(decl->kids.front().get());
                    symbol.detail = (return_type.empty() ? "" : return_type + " ") + decl->name +
                                    ParameterListText(decl) +
                                    (decl->trailing_qualifiers.empty() ? "" : " " + decl->trailing_qualifiers);
                    out->push_back(symbol);
                    break;
                }
                case CppNodeKind::Variable:
                case CppNodeKind::Field: {
                    if (decl->name.empty()) break;
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = decl->kind == CppNodeKind::Field ? CppSymbolKind::Field : CppSymbolKind::Variable;
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    const CppNode *type = decl->kids.empty() ? nullptr : decl->kids.front().get();
                    symbol.type_base = TypeBaseOf(type);
                    symbol.detail = CppTypeText(type) + " " + symbol.qualified;
                    out->push_back(symbol);
                    break;
                }
                case CppNodeKind::Typedef:
                case CppNodeKind::UsingAlias: {
                    if (decl->name.empty()) break;
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = CppSymbolKind::Typedef;
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    symbol.detail = "using " + decl->name + " = " +
                                    (decl->kids.empty() ? "" : CppTypeText(decl->kids.front().get()));
                    out->push_back(symbol);
                    break;
                }
                case CppNodeKind::Concept: {
                    HeaderSymbol symbol;
                    symbol.name = decl->name;
                    symbol.qualified = qualifier.empty() ? decl->name : qualifier + "::" + decl->name;
                    symbol.kind = CppSymbolKind::Concept;
                    symbol.line = decl->name_pos.line;
                    symbol.col = decl->name_pos.col;
                    symbol.end_col = decl->name_end.col;
                    out->push_back(symbol);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // --- Declarations -------------------------------------------------

    void WalkDeclarations(const std::vector<CppNodePtr> &body, int scope) {
        for (const CppNodePtr &child : body) WalkDeclaration(child.get(), scope);
    }

    void WalkDeclaration(const CppNode *node, int scope) {
        if (node == nullptr) return;
        switch (node->kind) {
            case CppNodeKind::Namespace: {
                int inner = FindNamespaceScope(scope, node->name);
                if (inner < 0) {
                    inner = NewScope(CppScope::Kind::Namespace, scope, node->name, node);
                    if (node->name.empty() || node->str_value == "inline") {
                        index_.scopes[static_cast<size_t>(scope)].transparent_children.push_back(inner);
                    }
                    CppSymbol symbol;
                    symbol.name = node->name;
                    symbol.kind = CppSymbolKind::Namespace;
                    symbol.node = node;
                    symbol.pos = node->name_pos;
                    symbol.end = node->name_end;
                    symbol.range_start = node->start;
                    symbol.range_end = node->end;
                    symbol.scope = scope;
                    symbol.child_scope = inner;
                    symbol.detail = "namespace " + node->name;
                    const int created = AddSymbol(std::move(symbol));
                    index_.scopes[static_cast<size_t>(inner)].owner = created;
                    if (!node->name.empty()) {
                        RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                    }
                } else {
                    index_.scopes[static_cast<size_t>(inner)].end = node->end;
                }
                WalkDeclarations(node->body, inner);
                break;
            }
            case CppNodeKind::NamespaceAlias: {
                CppSymbol symbol;
                symbol.name = node->name;
                symbol.kind = CppSymbolKind::Namespace;
                symbol.node = node;
                symbol.pos = node->name_pos;
                symbol.end = node->name_end;
                symbol.range_start = node->start;
                symbol.range_end = node->end;
                symbol.scope = scope;
                symbol.detail = "namespace " + node->name + " = " + node->str_value;
                const int created = AddSymbol(std::move(symbol));
                RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                break;
            }
            case CppNodeKind::UsingDirective: {
                index_.scopes[static_cast<size_t>(scope)].using_directives.push_back(node->name);
                if (CppLookupName(index_, scope, LastComponent(node->name)) < 0 && node->name != "std") {
                    index_.has_unknown_using_directive = true;
                }
                RecordOccurrence(LastComponent(node->name), node->name_pos, node->name_end, -1, false, false, false);
                break;
            }
            case CppNodeKind::UsingDecl: {
                // `using Base::method;` and `using std::string;` both make
                // a name visible here; the symbol it aliases is recorded
                // when it can be found.
                const int target = CppLookupName(index_, scope, node->name);
                CppSymbol symbol;
                symbol.name = node->name;
                symbol.kind = target >= 0 ? index_.symbols[static_cast<size_t>(target)].kind : CppSymbolKind::Typedef;
                symbol.node = node;
                symbol.pos = node->name_pos;
                symbol.end = node->name_end;
                symbol.range_start = node->start;
                symbol.range_end = node->end;
                symbol.scope = scope;
                symbol.detail = "using " + node->str_value;
                if (target >= 0) {
                    symbol.type_base = index_.symbols[static_cast<size_t>(target)].type_base;
                    symbol.child_scope = index_.symbols[static_cast<size_t>(target)].child_scope;
                }
                const int created = AddSymbol(std::move(symbol));
                RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                break;
            }
            case CppNodeKind::UsingAlias:
            case CppNodeKind::Typedef: {
                CppSymbol symbol;
                symbol.name = node->name;
                symbol.kind = CppSymbolKind::Typedef;
                symbol.node = node;
                symbol.pos = node->name_pos;
                symbol.end = node->name_end;
                symbol.range_start = node->start;
                symbol.range_end = node->end;
                symbol.scope = scope;
                const CppNode *type = node->kids.empty() ? nullptr : node->kids.front().get();
                symbol.type_text = CppTypeText(type);
                symbol.type_base = TypeBaseOf(type);
                symbol.detail = (node->kind == CppNodeKind::Typedef ? "typedef " : "using ") + symbol.type_text +
                                " " + node->name;
                symbol.doc = DocFor(node);
                const int created = AddSymbol(std::move(symbol));
                RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                if (type != nullptr) ResolveTypeReference(type, scope);
                break;
            }
            case CppNodeKind::Class: {
                WalkClass(node, scope);
                break;
            }
            case CppNodeKind::Enum: {
                WalkEnum(node, scope);
                break;
            }
            case CppNodeKind::Function: {
                WalkFunction(node, scope);
                break;
            }
            case CppNodeKind::Variable:
            case CppNodeKind::Field: {
                WalkVariable(node, scope);
                break;
            }
            case CppNodeKind::Concept: {
                CppSymbol symbol;
                symbol.name = node->name;
                symbol.kind = CppSymbolKind::Concept;
                symbol.node = node;
                symbol.pos = node->name_pos;
                symbol.end = node->name_end;
                symbol.range_start = node->start;
                symbol.range_end = node->end;
                symbol.scope = scope;
                symbol.detail = "concept " + node->name;
                symbol.doc = DocFor(node);
                const int created = AddSymbol(std::move(symbol));
                RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                break;
            }
            case CppNodeKind::LinkageSpec:
                WalkDeclarations(node->body, scope);
                break;
            case CppNodeKind::Friend:
                for (const CppNodePtr &child : node->kids) WalkDeclaration(child.get(), scope);
                break;
            case CppNodeKind::StaticAssert:
                for (const CppNodePtr &child : node->kids) WalkExpression(child.get(), scope, false);
                break;
            case CppNodeKind::DeclStmt:
                for (const CppNodePtr &child : node->kids) WalkDeclaration(child.get(), scope);
                break;
            case CppNodeKind::ModuleDecl:
                // `import mep.path_util;` brings in names this server
                // cannot follow, so the name checks stand down.
                if (node->str_value == "import") index_.imports_module = true;
                break;
            case CppNodeKind::Access:
            case CppNodeKind::Empty:
            case CppNodeKind::AsmStmt:
                break;
            default:
                // A statement that turned up at declaration level (a
                // recovered fragment, or a macro's block): walk it so its
                // names are still resolved.
                WalkStatement(node, scope);
                break;
        }
    }

    static std::string LastComponent(const std::string &qualified) {
        const size_t at = qualified.rfind("::");
        return at == std::string::npos ? qualified : qualified.substr(at + 2);
    }

    int FindNamespaceScope(int parent, const std::string &name) const {
        if (name.empty()) return -1;
        for (int child : index_.scopes[static_cast<size_t>(parent)].children) {
            const CppScope &scope = index_.scopes[static_cast<size_t>(child)];
            if (scope.kind == CppScope::Kind::Namespace && scope.name == name) return child;
        }
        return -1;
    }

    void WalkClass(const CppNode *node, int scope) {
        const int class_scope = NewScope(CppScope::Kind::Class, scope, node->name, node);
        CppSymbol symbol;
        symbol.name = node->name;
        symbol.kind = KindForClass(node);
        symbol.node = node;
        symbol.pos = node->name_pos;
        symbol.end = node->name_end;
        symbol.range_start = node->start;
        symbol.range_end = node->end;
        symbol.scope = scope;
        symbol.child_scope = class_scope;
        symbol.is_definition = node->is_definition;
        symbol.is_template = node->is_template;
        std::string detail = node->str_value + " " + node->name;
        if (!node->bases.empty()) {
            detail += " : ";
            for (size_t i = 0; i < node->bases.size(); i++) {
                if (i > 0) detail += ", ";
                detail += node->bases[i]->name;
            }
        }
        symbol.detail = detail;
        symbol.doc = DocFor(node);
        const int created = AddSymbol(std::move(symbol));
        index_.scopes[static_cast<size_t>(class_scope)].owner = created;
        if (!node->name.empty()) {
            RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
        }
        std::vector<std::string> base_names;
        for (const CppNodePtr &base : node->bases) {
            if (base == nullptr) continue;
            base_names.push_back(base->name);
            const int resolved = CppLookupName(index_, scope, LastComponent(TypeBaseOfName(base->name)));
            RecordOccurrence(LastComponent(TypeBaseOfName(base->name)), base->name_pos, base->name_end, resolved,
                             false, false, false);
        }
        if (!base_names.empty()) pending_bases_.emplace_back(class_scope, base_names);
        for (const CppNodePtr &param : node->template_params) AddTemplateParam(param.get(), class_scope);
        if (node->is_definition) WalkDeclarations(node->body, class_scope);
    }

    static std::string TypeBaseOfName(const std::string &spelled) {
        const size_t angle = spelled.find('<');
        return angle == std::string::npos ? spelled : spelled.substr(0, angle);
    }

    void WalkEnum(const CppNode *node, int scope) {
        CppSymbol symbol;
        symbol.name = node->name;
        symbol.kind = CppSymbolKind::Enum;
        symbol.node = node;
        symbol.pos = node->name_pos;
        symbol.end = node->name_end;
        symbol.range_start = node->start;
        symbol.range_end = node->end;
        symbol.scope = scope;
        symbol.detail = node->str_value + (node->name.empty() ? "" : " " + node->name);
        symbol.doc = DocFor(node);
        symbol.is_definition = node->is_definition;
        const int created = AddSymbol(std::move(symbol));
        if (!node->name.empty()) {
            RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
        }
        // A scoped enum's enumerators are reached through its name; an
        // unscoped one's leak into the enclosing scope. Both are recorded
        // in the enclosing scope, because that is where a lookup for
        // `Color::Red` and for `Red` both start.
        const int enum_scope = node->str_value == "enum class"
                                   ? NewScope(CppScope::Kind::Enum, scope, node->name, node)
                                   : scope;
        if (enum_scope != scope) {
            index_.symbols[static_cast<size_t>(created)].child_scope = enum_scope;
            index_.scopes[static_cast<size_t>(enum_scope)].owner = created;
        }
        for (const CppNodePtr &child : node->body) {
            if (child == nullptr || child->kind != CppNodeKind::Enumerator) continue;
            CppSymbol enumerator;
            enumerator.name = child->name;
            enumerator.kind = CppSymbolKind::Enumerator;
            enumerator.node = child.get();
            enumerator.pos = child->name_pos;
            enumerator.end = child->name_end;
            enumerator.range_start = child->start;
            enumerator.range_end = child->end;
            enumerator.scope = enum_scope;
            enumerator.type_base = node->name;
            enumerator.detail = (node->name.empty() ? "" : node->name + "::") + child->name;
            enumerator.doc = DocFor(child.get());
            const int enum_symbol = AddSymbol(std::move(enumerator));
            RecordOccurrence(child->name, child->name_pos, child->name_end, enum_symbol, false, true, false);
            for (const CppNodePtr &value : child->kids) WalkExpression(value.get(), scope, false);
        }
    }

    void AddTemplateParam(const CppNode *param, int scope) {
        if (param == nullptr || param->name.empty()) return;
        CppSymbol symbol;
        symbol.name = param->name;
        symbol.kind = CppSymbolKind::TemplateParam;
        symbol.node = param;
        symbol.pos = param->name_pos;
        symbol.end = param->name_end;
        symbol.range_start = param->start;
        symbol.range_end = param->end;
        symbol.scope = scope;
        symbol.detail = param->str_value == "type" ? "typename " + param->name : param->name;
        const int created = AddSymbol(std::move(symbol));
        RecordOccurrence(param->name, param->name_pos, param->name_end, created, false, true, false);
    }

    void WalkFunction(const CppNode *node, int scope) {
        // An out-of-class definition (`void Editor::Draw()`) belongs to
        // the class's scope, not to the file's: that is what makes its
        // body see the class's members.
        int owner_scope = scope;
        const size_t qualifier_end = node->qualified.rfind("::");
        if (qualifier_end != std::string::npos) {
            const std::string qualifier = node->qualified.substr(0, qualifier_end);
            const int owner = CppLookupName(index_, scope, LastComponent(TypeBaseOfName(qualifier)));
            if (owner >= 0 && index_.symbols[static_cast<size_t>(owner)].child_scope >= 0) {
                owner_scope = index_.symbols[static_cast<size_t>(owner)].child_scope;
                RecordOccurrence(LastComponent(TypeBaseOfName(qualifier)), node->start, node->start, owner, false,
                                 false, false);
            }
        }
        const CppScope::Kind owner_kind = index_.scopes[static_cast<size_t>(owner_scope)].kind;
        CppSymbol symbol;
        symbol.name = node->name;
        symbol.qualified = node->qualified.empty() ? "" : node->qualified;
        symbol.node = node;
        symbol.pos = node->name_pos;
        symbol.end = node->name_end;
        symbol.range_start = node->start;
        symbol.range_end = node->end;
        symbol.scope = owner_scope;
        symbol.is_static = node->is_static;
        symbol.is_virtual = node->is_virtual;
        symbol.is_pure = node->is_pure;
        symbol.is_const = node->is_const;
        symbol.is_definition = node->is_definition;
        symbol.is_template = node->is_template;
        symbol.is_extern = node->is_extern;
        symbol.is_deleted = node->is_deleted;
        symbol.is_defaulted = node->is_defaulted;
        symbol.is_constexpr = node->is_constexpr;
        symbol.access = node->str_value;
        const std::string return_type = node->kids.empty() ? "" : CppTypeText(node->kids.front().get());
        symbol.type_text = return_type;
        symbol.type_base = node->kids.empty() ? "" : TypeBaseOf(node->kids.front().get());
        symbol.detail = (return_type.empty() ? "" : return_type + " ") + node->name + ParameterListText(node) +
                        (node->trailing_qualifiers.empty() ? "" : " " + node->trailing_qualifiers);
        symbol.doc = DocFor(node);
        if (owner_kind == CppScope::Kind::Class) {
            const std::string &class_name = index_.scopes[static_cast<size_t>(owner_scope)].name;
            if (node->name == class_name) {
                symbol.kind = CppSymbolKind::Constructor;
            } else if (node->name == "~" + class_name || node->name.rfind("~", 0) == 0) {
                symbol.kind = CppSymbolKind::Destructor;
            } else {
                symbol.kind = CppSymbolKind::Method;
            }
        } else {
            symbol.kind = CppSymbolKind::Function;
        }
        const int created = AddSymbol(std::move(symbol));
        if (!node->name.empty()) {
            RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
        }

        const int function_scope = NewScope(CppScope::Kind::Function, owner_scope, "", node);
        if (owner_scope != scope) index_.scopes[static_cast<size_t>(function_scope)].lexical_parent = scope;
        index_.symbols[static_cast<size_t>(created)].child_scope = function_scope;
        index_.scopes[static_cast<size_t>(function_scope)].owner = created;
        for (const CppNodePtr &param : node->template_params) AddTemplateParam(param.get(), function_scope);
        for (const CppNodePtr &param : node->params) {
            if (param == nullptr) continue;
            const CppNode *type = param->kids.empty() ? nullptr : param->kids.front().get();
            if (type != nullptr) ResolveTypeReference(type, function_scope);
            if (param->name.empty()) continue;
            CppSymbol parameter;
            parameter.name = param->name;
            parameter.kind = CppSymbolKind::Parameter;
            parameter.node = param.get();
            parameter.pos = param->name_pos;
            parameter.end = param->name_end;
            parameter.range_start = param->start;
            parameter.range_end = param->end;
            parameter.scope = function_scope;
            parameter.type_text = CppTypeText(type);
            parameter.type_base = TypeBaseOf(type);
            parameter.ptr_depth = type == nullptr ? 0 : type->ptr_depth;
            parameter.is_ref = type != nullptr && type->is_ref;
            parameter.detail = parameter.type_text + " " + param->name;
            const int param_symbol = AddSymbol(std::move(parameter));
            RecordOccurrence(param->name, param->name_pos, param->name_end, param_symbol, false, true, false);
            for (size_t i = 1; i < param->kids.size(); i++) {
                WalkExpression(param->kids[i].get(), function_scope, false);
            }
        }
        if (!node->kids.empty() && node->kids.front() != nullptr) {
            ResolveTypeReference(node->kids.front().get(), owner_scope);
        }
        // Member initializers: the member names resolve in the class, the
        // arguments in the function.
        for (size_t i = 1; i < node->kids.size(); i++) {
            const CppNode *init = node->kids[i].get();
            if (init == nullptr || init->kind != CppNodeKind::MemberInit) continue;
            const int member = owner_kind == CppScope::Kind::Class
                                   ? CppLookupMember(index_, owner_scope, init->name)
                                   : -1;
            RecordOccurrence(init->name, init->name_pos, init->name_end, member, true, false, false, -1,
                             owner_scope);
            for (const CppNodePtr &arg : init->kids) WalkExpression(arg.get(), function_scope, false);
        }
        for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), function_scope);
        for (const CppNodePtr &handler : node->handlers) WalkStatement(handler.get(), function_scope);
    }

    void WalkVariable(const CppNode *node, int scope) {
        const CppNode *type = node->kids.empty() ? nullptr : node->kids.front().get();
        CppSymbol symbol;
        symbol.name = node->name;
        symbol.qualified = node->qualified.empty() ? "" : node->qualified;
        symbol.kind = index_.scopes[static_cast<size_t>(scope)].kind == CppScope::Kind::Class
                          ? CppSymbolKind::Field
                          : CppSymbolKind::Variable;
        symbol.node = node;
        symbol.pos = node->name_pos;
        symbol.end = node->name_end;
        symbol.range_start = node->start;
        symbol.range_end = node->end;
        symbol.scope = scope;
        symbol.type_text = CppTypeText(type);
        symbol.type_base = TypeBaseOf(type);
        symbol.ptr_depth = type == nullptr ? 0 : type->ptr_depth;
        symbol.is_ref = type != nullptr && type->is_ref;
        symbol.is_static = node->is_static;
        symbol.is_extern = node->is_extern;
        symbol.is_constexpr = node->is_constexpr;
        symbol.is_const = type != nullptr && type->is_const;
        symbol.access = node->str_value;
        symbol.detail = symbol.type_text + (symbol.type_text.empty() ? "" : " ") + node->name;
        symbol.doc = DocFor(node);
        // `auto x = Point(1, 2);` and `auto *p = new Node;` say what they
        // are outright; nothing else is deduced (see cpp_lsp.h).
        if (symbol.type_base == "auto" && node->kids.size() > 1) {
            const std::string deduced = DeducedTypeBase(node->kids[1].get(), scope);
            if (!deduced.empty()) symbol.type_base = deduced;
        }
        const int created = AddSymbol(std::move(symbol));
        RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
        if (type != nullptr) ResolveTypeReference(type, scope);
        for (size_t i = 1; i < node->kids.size(); i++) WalkExpression(node->kids[i].get(), scope, false);
    }

    /** @brief The class an `auto` variable's initializer names outright, or "". */
    std::string DeducedTypeBase(const CppNode *init, int scope) const {
        if (init == nullptr) return "";
        if (init->kind == CppNodeKind::Call && !init->kids.empty()) {
            const CppNode *callee = init->kids.front().get();
            if (callee != nullptr && callee->kind == CppNodeKind::Id) {
                // `auto p = Point(1, 2)` names a type; `auto &c = Cache()`
                // names a function, and what it deduces is that
                // function's return type -- not the function.
                const int symbol = CppLookupName(index_, scope, LastComponent(callee->str_value));
                if (symbol >= 0) {
                    const CppSymbol &target = index_.symbols[static_cast<size_t>(symbol)];
                    if (target.kind == CppSymbolKind::Function || target.kind == CppSymbolKind::Method) {
                        return target.type_base;
                    }
                }
                return callee->str_value;
            }
        }
        if (init->kind == CppNodeKind::New && !init->kids.empty()) {
            return TypeBaseOf(init->kids.front().get());
        }
        if (init->kind == CppNodeKind::Cast && !init->kids.empty()) {
            return TypeBaseOf(init->kids.front().get());
        }
        return "";
    }

    /** @brief Records the class a written type names, so the type itself counts as a use of it. */
    void ResolveTypeReference(const CppNode *type, int scope) {
        if (type == nullptr) return;
        const std::string base = TypeBaseOf(type);
        if (base.empty() || IsFundamentalSpelling(base)) return;
        const std::string last = LastComponent(base);
        const int symbol = CppLookupName(index_, scope, last);
        if (symbol < 0) return;
        index_.symbols[static_cast<size_t>(symbol)].reads++;
    }

    // --- Statements ---------------------------------------------------

    void WalkStatement(const CppNode *node, int scope) {
        if (node == nullptr) return;
        switch (node->kind) {
            case CppNodeKind::Compound: {
                const int block = NewScope(CppScope::Kind::Block, scope, "", node);
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), block);
                break;
            }
            case CppNodeKind::DeclStmt:
                for (const CppNodePtr &decl : node->kids) WalkDeclaration(decl.get(), scope);
                break;
            case CppNodeKind::ExprStmt:
                for (const CppNodePtr &expr : node->kids) WalkExpression(expr.get(), scope, false);
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), scope);
                break;
            case CppNodeKind::If:
            case CppNodeKind::While:
            case CppNodeKind::Switch:
            case CppNodeKind::DoWhile: {
                const int block = NewScope(CppScope::Kind::Block, scope, "", node);
                for (const CppNodePtr &kid : node->kids) {
                    if (kid == nullptr) continue;
                    if (kid->kind == CppNodeKind::Variable || kid->kind == CppNodeKind::Field ||
                        kid->kind == CppNodeKind::DeclStmt || kid->kind == CppNodeKind::ExprStmt) {
                        WalkStatement(kid.get(), block);
                    } else {
                        WalkExpression(kid.get(), block, false);
                    }
                }
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), block);
                for (const CppNodePtr &stmt : node->orelse) WalkStatement(stmt.get(), block);
                break;
            }
            case CppNodeKind::For:
            case CppNodeKind::RangeFor: {
                const int block = NewScope(CppScope::Kind::Block, scope, "", node);
                for (const CppNodePtr &kid : node->kids) {
                    if (kid == nullptr) continue;
                    if (kid->kind == CppNodeKind::Variable || kid->kind == CppNodeKind::DeclStmt ||
                        kid->kind == CppNodeKind::Field) {
                        WalkDeclaration(kid.get(), block);
                    } else if (kid->kind == CppNodeKind::ExprStmt) {
                        WalkStatement(kid.get(), block);
                    } else {
                        WalkExpression(kid.get(), block, false);
                    }
                }
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), block);
                break;
            }
            case CppNodeKind::Case:
            case CppNodeKind::Default:
            case CppNodeKind::Return:
            case CppNodeKind::CoReturn:
                for (const CppNodePtr &kid : node->kids) WalkExpression(kid.get(), scope, false);
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), scope);
                break;
            case CppNodeKind::Try: {
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), scope);
                for (const CppNodePtr &handler : node->handlers) WalkStatement(handler.get(), scope);
                break;
            }
            case CppNodeKind::Catch: {
                const int block = NewScope(CppScope::Kind::Block, scope, "", node);
                for (const CppNodePtr &kid : node->kids) {
                    if (kid == nullptr || kid->kind != CppNodeKind::Param) continue;
                    const CppNode *type = kid->kids.empty() ? nullptr : kid->kids.front().get();
                    if (!kid->name.empty()) {
                        CppSymbol symbol;
                        symbol.name = kid->name;
                        symbol.kind = CppSymbolKind::Variable;
                        symbol.node = kid.get();
                        symbol.pos = kid->name_pos;
                        symbol.end = kid->name_end;
                        symbol.range_start = kid->start;
                        symbol.range_end = kid->end;
                        symbol.scope = block;
                        symbol.type_text = CppTypeText(type);
                        symbol.type_base = TypeBaseOf(type);
                        symbol.detail = symbol.type_text + " " + kid->name;
                        // A caught exception nobody reads is idiomatic,
                        // not a mistake: `catch (const Error &e) {}` is
                        // how you say "any error".
                        symbol.reads = 1;
                        const int created = AddSymbol(std::move(symbol));
                        RecordOccurrence(kid->name, kid->name_pos, kid->name_end, created, false, true, false);
                    }
                    if (type != nullptr) ResolveTypeReference(type, block);
                }
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), block);
                break;
            }
            case CppNodeKind::Label: {
                CppSymbol symbol;
                symbol.name = node->name;
                symbol.kind = CppSymbolKind::Label;
                symbol.node = node;
                symbol.pos = node->name_pos;
                symbol.end = node->name_end;
                symbol.range_start = node->start;
                symbol.range_end = node->end;
                symbol.scope = scope;
                symbol.reads = 1;  // a label is used by a `goto` elsewhere
                const int created = AddSymbol(std::move(symbol));
                RecordOccurrence(node->name, node->name_pos, node->name_end, created, false, true, false);
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), scope);
                break;
            }
            case CppNodeKind::Goto: {
                const int target = CppLookupName(index_, scope, node->name);
                RecordOccurrence(node->name, node->name_pos, node->name_end, target, false, false, false, -1,
                                 scope);
                break;
            }
            case CppNodeKind::Variable:
            case CppNodeKind::Field:
            case CppNodeKind::Class:
            case CppNodeKind::Enum:
            case CppNodeKind::Function:
            case CppNodeKind::Typedef:
            case CppNodeKind::UsingAlias:
            case CppNodeKind::UsingDecl:
            case CppNodeKind::UsingDirective:
            case CppNodeKind::StaticAssert:
            case CppNodeKind::Namespace:
                WalkDeclaration(node, scope);
                break;
            case CppNodeKind::Break:
            case CppNodeKind::Continue:
            case CppNodeKind::Empty:
            case CppNodeKind::AsmStmt:
                break;
            default:
                WalkExpression(node, scope, false);
                break;
        }
    }

    // --- Expressions --------------------------------------------------

    void WalkExpression(const CppNode *node, int scope, bool write) {
        if (node == nullptr) return;
        switch (node->kind) {
            case CppNodeKind::Id: {
                const std::string last = LastComponent(node->str_value);
                int symbol = -1;
                if (node->str_value.find("::") != std::string::npos) {
                    symbol = ResolveQualified(node->str_value, scope);
                } else {
                    symbol = CppLookupName(index_, scope, last);
                }
                RecordOccurrence(node->name.empty() ? last : node->name, node->name_pos, node->name_end, symbol,
                                 write, false, node->str_value.find("::") != std::string::npos, -1, scope,
                                 read_write_);
                break;
            }
            case CppNodeKind::Member: {
                WalkExpression(node->kids.empty() ? nullptr : node->kids.front().get(), scope, false);
                int owner = -1;
                const int member = ResolveMemberOf(node, scope, &owner);
                RecordOccurrence(node->name, node->name_pos, node->name_end, member, write, false, true, owner,
                                 scope, read_write_);
                break;
            }
            case CppNodeKind::Binary: {
                const bool compound = node->name.size() >= 2 && node->name.back() == '=' && node->name != "==" &&
                                      node->name != "!=" && node->name != "<=" && node->name != ">=";
                const bool assignment = node->name == "=" || compound;
                // A compound assignment reads the target as well.
                const bool saved_read_write = read_write_;
                read_write_ = compound;
                if (!node->kids.empty()) WalkExpression(node->kids.front().get(), scope, assignment);
                read_write_ = saved_read_write;
                for (size_t i = 1; i < node->kids.size(); i++) WalkExpression(node->kids[i].get(), scope, false);
                break;
            }
            case CppNodeKind::Unary: {
                const bool mutates = node->name == "++" || node->name == "--";
                const bool address = node->name == "&";
                // `*p = v` writes through `p` and reads `p` itself.
                const bool deref = node->name == "*";
                const bool saved_read_write = read_write_;
                if (mutates) read_write_ = true;
                if (!node->kids.empty()) {
                    WalkExpression(node->kids.front().get(), scope, !deref && (mutates || write));
                }
                read_write_ = saved_read_write;
                if (address && !node->kids.empty()) MarkAddressTaken(node->kids.front().get(), scope);
                break;
            }
            case CppNodeKind::Call: {
                for (size_t i = 0; i < node->kids.size(); i++) WalkExpression(node->kids[i].get(), scope, false);
                break;
            }
            case CppNodeKind::Lambda: {
                // The captures are evaluated in the enclosing scope, and
                // the trailing return type with them; only the
                // parameters and the body belong to the lambda's own.
                for (const CppNodePtr &capture : node->kids) {
                    if (capture != nullptr && capture->kind != CppNodeKind::Type) {
                        WalkExpression(capture.get(), scope, false);
                    }
                }
                const int lambda_scope = NewScope(CppScope::Kind::Function, scope, "", node);
                for (const CppNodePtr &param : node->params) {
                    if (param == nullptr || param->name.empty()) continue;
                    const CppNode *type = param->kids.empty() ? nullptr : param->kids.front().get();
                    CppSymbol symbol;
                    symbol.name = param->name;
                    symbol.kind = CppSymbolKind::Parameter;
                    symbol.node = param.get();
                    symbol.pos = param->name_pos;
                    symbol.end = param->name_end;
                    symbol.range_start = param->start;
                    symbol.range_end = param->end;
                    symbol.scope = lambda_scope;
                    symbol.type_text = CppTypeText(type);
                    symbol.type_base = TypeBaseOf(type);
                    symbol.detail = symbol.type_text + " " + param->name;
                    const int created = AddSymbol(std::move(symbol));
                    RecordOccurrence(param->name, param->name_pos, param->name_end, created, false, true, false);
                }
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), lambda_scope);
                break;
            }
            case CppNodeKind::Cast:
            case CppNodeKind::New:
            case CppNodeKind::SizeOf: {
                for (const CppNodePtr &kid : node->kids) {
                    if (kid == nullptr) continue;
                    if (kid->kind == CppNodeKind::Type) {
                        ResolveTypeReference(kid.get(), scope);
                        const std::string base = LastComponent(TypeBaseOf(kid.get()));
                        if (!base.empty() && !IsFundamentalSpelling(TypeBaseOf(kid.get()))) {
                            const int symbol = CppLookupName(index_, scope, base);
                            RecordOccurrence(base, kid->start, kid->start, symbol, false, false, false, -1, scope);
                        }
                    } else {
                        WalkExpression(kid.get(), scope, false);
                    }
                }
                break;
            }
            case CppNodeKind::Type: {
                ResolveTypeReference(node, scope);
                break;
            }
            case CppNodeKind::Subscript: {
                // `planes[ci][idx] = v` writes through the array and
                // *reads* everything it names -- the array pointer and
                // both indices. Propagating the write into them is what
                // made `unsigned char *dst = ...; dst[0] = v;` look like
                // a pointer nobody read.
                for (const CppNodePtr &kid : node->kids) WalkExpression(kid.get(), scope, false);
                break;
            }
            case CppNodeKind::Paren:
                for (const CppNodePtr &kid : node->kids) WalkExpression(kid.get(), scope, write);
                break;
            case CppNodeKind::Literal:
            case CppNodeKind::This:
            case CppNodeKind::Placeholder:
                break;
            default:
                for (const CppNodePtr &kid : node->kids) WalkExpression(kid.get(), scope, false);
                for (const CppNodePtr &stmt : node->body) WalkStatement(stmt.get(), scope);
                break;
        }
    }

    void MarkAddressTaken(const CppNode *node, int scope) {
        if (node == nullptr || node->kind != CppNodeKind::Id) return;
        const int symbol = CppLookupName(index_, scope, LastComponent(node->str_value));
        if (symbol >= 0) index_.symbols[static_cast<size_t>(symbol)].address_taken = true;
    }

    /** @brief Resolves `a::b::c` by walking the qualifiers that name scopes in this file. */
    int ResolveQualified(const std::string &qualified, int scope) const {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= qualified.size()) {
            const size_t at = qualified.find("::", start);
            if (at == std::string::npos) {
                parts.push_back(qualified.substr(start));
                break;
            }
            parts.push_back(qualified.substr(start, at - start));
            start = at + 2;
        }
        // Strip template arguments from each component.
        for (std::string &part : parts) {
            const size_t angle = part.find('<');
            if (angle != std::string::npos) part = part.substr(0, angle);
            part = Trim(part);
        }
        if (parts.empty()) return -1;
        size_t index = 0;
        if (parts.front().empty()) index = 1;  // a leading `::`
        if (index >= parts.size()) return -1;
        int symbol = CppLookupName(index_, scope, parts[index]);
        index++;
        while (symbol >= 0 && index < parts.size()) {
            const int child = index_.symbols[static_cast<size_t>(symbol)].child_scope;
            if (child < 0) return -1;
            symbol = CppLookupMember(index_, child, parts[index]);
            index++;
        }
        return symbol;
    }

    /** @brief Resolves the member of a `.`/`->` access, when the object's type is known here. */
    int ResolveMemberOf(const CppNode *node, int scope, int *out_owner_type = nullptr) const {
        if (node->kids.empty()) return -1;
        const std::string type_base = TypeOfExpression(node->kids.front().get(), scope);
        if (type_base.empty()) return -1;
        const int type_symbol = CppResolveTypeName(index_, scope, type_base);
        if (type_symbol < 0) return -1;
        const int class_scope = index_.symbols[static_cast<size_t>(type_symbol)].child_scope;
        if (class_scope < 0) return -1;
        if (out_owner_type != nullptr) *out_owner_type = type_symbol;
        return CppLookupMember(index_, class_scope, node->name);
    }

    /** @brief The bare type name of an expression, where the file says so outright. */
    std::string TypeOfExpression(const CppNode *node, int scope) const {
        if (node == nullptr) return "";
        switch (node->kind) {
            case CppNodeKind::Id: {
                const int symbol = CppLookupName(index_, scope, LastComponent(node->str_value));
                if (symbol < 0) return "";
                return index_.symbols[static_cast<size_t>(symbol)].type_base;
            }
            case CppNodeKind::This: {
                for (int current = scope; current >= 0;
                     current = index_.scopes[static_cast<size_t>(current)].parent) {
                    if (index_.scopes[static_cast<size_t>(current)].kind == CppScope::Kind::Class) {
                        return index_.scopes[static_cast<size_t>(current)].name;
                    }
                }
                return "";
            }
            case CppNodeKind::Member: {
                const int member = ResolveMemberOf(node, scope);
                if (member < 0) return "";
                return index_.symbols[static_cast<size_t>(member)].type_base;
            }
            case CppNodeKind::Paren:
                return node->kids.empty() ? "" : TypeOfExpression(node->kids.front().get(), scope);
            case CppNodeKind::Unary:
                // `*p` and `p->` both reach the pointee, which for this
                // model is the same written type.
                return node->kids.empty() ? "" : TypeOfExpression(node->kids.front().get(), scope);
            case CppNodeKind::Call: {
                if (node->kids.empty()) return "";
                const CppNode *callee = node->kids.front().get();
                if (callee == nullptr) return "";
                const int symbol = callee->kind == CppNodeKind::Id
                                       ? CppLookupName(index_, scope, LastComponent(callee->str_value))
                                       : -1;
                if (symbol < 0) return "";
                const CppSymbol &target = index_.symbols[static_cast<size_t>(symbol)];
                // A constructor call names its own type.
                if (target.kind == CppSymbolKind::Class || target.kind == CppSymbolKind::Struct) {
                    return target.name;
                }
                return target.type_base;
            }
            default:
                return "";
        }
    }

    // --- Comments -----------------------------------------------------

    std::string DocFor(const CppNode *node) const {
        if (node == nullptr) return "";
        return CppDocCommentAbove(index_, node->start.line);
    }

    /**
     * @brief Retries every name that did not resolve on the first walk.
     *
     * C++ is not a one-pass language from a reader's point of view: a
     * member function's body sees members declared below it, and a call
     * at file scope may name a function defined later in the file. The
     * first walk resolves what it can as it goes; this pass fills in the
     * rest now that every declaration is known.
     */
    void ResolveForwardReferences() {
        for (CppOccurrence &occurrence : index_.occurrences) {
            if (occurrence.symbol >= 0 || occurrence.is_declaration || occurrence.name.empty()) continue;
            int symbol = -1;
            if (occurrence.is_member) {
                if (occurrence.owner_type < 0) continue;
                const int class_scope = index_.symbols[static_cast<size_t>(occurrence.owner_type)].child_scope;
                if (class_scope < 0) continue;
                symbol = CppLookupMember(index_, class_scope, occurrence.name);
            } else {
                symbol = CppLookupName(index_, occurrence.scope, occurrence.name);
            }
            if (symbol < 0) continue;
            occurrence.symbol = symbol;
            CppSymbol &target = index_.symbols[static_cast<size_t>(symbol)];
            if (occurrence.is_write) {
                target.writes++;
            } else {
                target.reads++;
            }
        }
    }

    void ResolvePendingBases() {
        for (const auto &entry : pending_bases_) {
            CppScope &scope = index_.scopes[static_cast<size_t>(entry.first)];
            for (const std::string &base : entry.second) {
                const std::string last = LastComponent(TypeBaseOfName(base));
                const int symbol = CppLookupName(index_, scope.parent, last);
                if (symbol >= 0 && index_.symbols[static_cast<size_t>(symbol)].child_scope >= 0) {
                    scope.base_scopes.push_back(index_.symbols[static_cast<size_t>(symbol)].child_scope);
                } else {
                    scope.unresolved_bases.push_back(base);
                }
            }
        }
    }
};

}  // namespace

// --- Public index API -------------------------------------------------

CppIndex BuildCppIndex(const std::vector<std::string> &lines, const CppLspOptions &opts) {
    CppIndex index;
    index.lines = lines;
    CppParseOptions parse_opts;
    parse_opts.doc_dir = opts.doc_dir;
    parse_opts.include_dirs = opts.include_dirs;
    parse_opts.resolve_includes = opts.check_files && !opts.doc_dir.empty();
    parse_opts.defines = opts.defines;
    index.parse = ParseCpp(lines, parse_opts);
    index.truncated = index.parse.truncated;
    const std::string &name = opts.file_name;
    index.file_is_header = EndsWith(name, ".h") || EndsWith(name, ".hpp") || EndsWith(name, ".hh") ||
                           EndsWith(name, ".hxx") || EndsWith(name, ".inl") ||
                           (!name.empty() && name.find('.') == std::string::npos);
    IndexBuilder builder(index, opts);
    builder.Build();
    return index;
}

int CppScopeAt(const CppIndex &index, const CppPos &pos) {
    int best = 0;
    for (size_t i = 0; i < index.scopes.size(); i++) {
        const CppScope &scope = index.scopes[i];
        if (scope.node == nullptr) continue;
        if (CppPosLess(pos, scope.start) || CppPosLess(scope.end, pos)) continue;
        // Innermost wins: scopes are created outermost first, so a later
        // index that still contains the position is deeper.
        if (static_cast<int>(i) > best) best = static_cast<int>(i);
    }
    return best;
}

const CppOccurrence *CppOccurrenceAt(const CppIndex &index, int line, int col) {
    const CppOccurrence *best = nullptr;
    for (const CppOccurrence &occurrence : index.occurrences) {
        if (occurrence.pos.line != line) continue;
        if (col < occurrence.pos.col || col > occurrence.pos.col + occurrence.length) continue;
        if (best == nullptr || occurrence.pos.col > best->pos.col) best = &occurrence;
    }
    return best;
}

int CppLookupMember(const CppIndex &index, int class_scope, const std::string &name) {
    if (class_scope < 0 || static_cast<size_t>(class_scope) >= index.scopes.size()) return -1;
    const CppScope &scope = index.scopes[static_cast<size_t>(class_scope)];
    for (int symbol : scope.symbols) {
        if (index.symbols[static_cast<size_t>(symbol)].name == name) return symbol;
    }
    for (int base : scope.base_scopes) {
        if (base == class_scope) continue;
        const int found = CppLookupMember(index, base, name);
        if (found >= 0) return found;
    }
    return -1;
}

namespace {

/** @brief Reports whether a symbol kind is one a type name can resolve to. */
bool IsTypeSymbol(CppSymbolKind kind) {
    return kind == CppSymbolKind::Class || kind == CppSymbolKind::Struct || kind == CppSymbolKind::Union ||
           kind == CppSymbolKind::Enum || kind == CppSymbolKind::Typedef || kind == CppSymbolKind::TemplateParam;
}

}  // namespace

int CppLookupName(const CppIndex &index, int scope, const std::string &name) {
    return CppLookupNameFiltered(index, scope, name, false);
}

int CppLookupNameFiltered(const CppIndex &index, int scope, const std::string &name, bool types_only) {
    if (name.empty()) return -1;
    // Two chains, in the order the language looks: the scopes this one
    // is nested in, then -- for an out-of-class definition -- the scopes
    // it was written in (see CppScope::lexical_parent).
    std::vector<int> starts{scope};
    for (size_t start = 0; start < starts.size() && start < 8; start++) {
        int current = starts[start];
        int guard = 0;
        while (current >= 0 && guard++ < 200) {
            const CppScope &here = index.scopes[static_cast<size_t>(current)];
            if (here.lexical_parent >= 0) starts.push_back(here.lexical_parent);
            // A class scope brings its bases with it.
            if (here.kind == CppScope::Kind::Class) {
                const int found = CppLookupMember(index, current, name);
                // A class's own constructor has the class's name: a
                // lookup for the *type* has to walk past it, or
                // `parser->depth_` inside a nested struct resolves the
                // type `Parser` to `Parser::Parser` and finds nothing.
                if (found >= 0 && (!types_only || IsTypeSymbol(index.symbols[static_cast<size_t>(found)].kind))) {
                    return found;
                }
            } else {
                for (int symbol : here.symbols) {
                    if (index.symbols[static_cast<size_t>(symbol)].name != name) continue;
                    if (types_only && !IsTypeSymbol(index.symbols[static_cast<size_t>(symbol)].kind)) continue;
                    return symbol;
                }
            }
            // An unnamed or inline namespace's names are this scope's names.
            for (int transparent : here.transparent_children) {
                for (int symbol : index.scopes[static_cast<size_t>(transparent)].symbols) {
                    if (index.symbols[static_cast<size_t>(symbol)].name != name) continue;
                    if (types_only && !IsTypeSymbol(index.symbols[static_cast<size_t>(symbol)].kind)) continue;
                    return symbol;
                }
            }
            for (const std::string &directive : here.using_directives) {
                // `using namespace X;` -- look the name up inside X, but
                // only one level: a directive inside X is a rabbit hole
                // nothing here needs.
                const size_t at = directive.rfind("::");
                const std::string last = at == std::string::npos ? directive : directive.substr(at + 2);
                for (const CppScope &candidate : index.scopes) {
                    if (candidate.kind != CppScope::Kind::Namespace || candidate.name != last) continue;
                    for (int symbol : candidate.symbols) {
                        if (index.symbols[static_cast<size_t>(symbol)].name == name) return symbol;
                    }
                }
            }
            current = here.parent;
        }
    }
    return -1;
}

int CppResolveTypeName(const CppIndex &index, int scope, const std::string &type_base) {
    if (type_base.empty()) return -1;
    const size_t at = type_base.rfind("::");
    const std::string last = at == std::string::npos ? type_base : type_base.substr(at + 2);
    int symbol = CppLookupNameFiltered(index, scope, last, true);
    int guard = 0;
    // A typedef or `using` alias resolves to what it names.
    while (symbol >= 0 && guard++ < 16) {
        const CppSymbol &found = index.symbols[static_cast<size_t>(symbol)];
        if (found.kind != CppSymbolKind::Typedef || found.type_base.empty()) return symbol;
        const size_t inner_at = found.type_base.rfind("::");
        const std::string inner = inner_at == std::string::npos ? found.type_base
                                                                : found.type_base.substr(inner_at + 2);
        const int next = CppLookupNameFiltered(index, found.scope, inner, true);
        if (next < 0 || next == symbol) return symbol;
        symbol = next;
    }
    return symbol;
}

bool CppPositionInLiteral(const CppIndex &index, int line, int col) {
    for (const CppComment &comment : index.parse.comments) {
        if (line < comment.line || line > comment.end_line) continue;
        if (line == comment.line && col < comment.col) continue;
        if (line == comment.end_line && col > comment.end_col) continue;
        return true;
    }
    for (const CppToken &token : index.parse.raw_tokens) {
        if (token.kind != CppTokKind::String && token.kind != CppTokKind::Char) continue;
        if (line < token.start.line || line > token.end.line) continue;
        if (line == token.start.line && col <= token.start.col) continue;
        if (line == token.end.line && col >= token.end.col) continue;
        return true;
    }
    return false;
}

std::string CppWordAt(const std::vector<std::string> &lines, int line, int col, int *out_start, int *out_end) {
    if (out_start != nullptr) *out_start = col;
    if (out_end != nullptr) *out_end = col;
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return "";
    const std::string &text = lines[static_cast<size_t>(line)];
    if (col < 0) return "";
    size_t position = std::min(static_cast<size_t>(col), text.size());
    // A cursor just past the end of a word still means that word.
    if (position > 0 && (position == text.size() || !IsCppIdentChar(static_cast<unsigned char>(text[position]))) &&
        IsCppIdentChar(static_cast<unsigned char>(text[position - 1]))) {
        position--;
    }
    if (position >= text.size() || !IsCppIdentChar(static_cast<unsigned char>(text[position]))) return "";
    size_t start = position;
    while (start > 0 && IsCppIdentChar(static_cast<unsigned char>(text[start - 1]))) start--;
    size_t end = position;
    while (end < text.size() && IsCppIdentChar(static_cast<unsigned char>(text[end]))) end++;
    if (out_start != nullptr) *out_start = static_cast<int>(start);
    if (out_end != nullptr) *out_end = static_cast<int>(end);
    return text.substr(start, end - start);
}

std::string CppDocCommentAbove(const CppIndex &index, int line) {
    // The comment block directly above the declaration, with its markers
    // stripped: `/** ... */`, a run of `///` lines, or a run of plain
    // `//` lines. Anything separated by a blank line belongs to whatever
    // came before it, not to this declaration.
    std::vector<const CppComment *> block;
    int want_line = line - 1;
    for (int guard = 0; guard < 200; guard++) {
        const CppComment *found = nullptr;
        for (const CppComment &comment : index.parse.comments) {
            if (!comment.own_line) continue;
            if (comment.end_line != want_line) continue;
            found = &comment;
        }
        if (found == nullptr) break;
        block.push_back(found);
        want_line = found->line - 1;
    }
    if (block.empty()) return "";
    std::string out;
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
        std::string text = (*it)->text;
        if ((*it)->block) {
            if (text.rfind("/*", 0) == 0) text = text.substr(2);
            if (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) {
                text = text.substr(0, text.size() - 2);
            }
            if (!text.empty() && (text.front() == '*' || text.front() == '!')) text = text.substr(1);
            // Strip the leading ` * ` of each continuation line.
            std::string cleaned;
            std::string current;
            for (size_t i = 0; i <= text.size(); i++) {
                if (i == text.size() || text[i] == '\n') {
                    std::string trimmed = Trim(current);
                    if (!trimmed.empty() && trimmed.front() == '*') trimmed = Trim(trimmed.substr(1));
                    if (!cleaned.empty()) cleaned += '\n';
                    cleaned += trimmed;
                    current.clear();
                    continue;
                }
                current += text[i];
            }
            text = cleaned;
        } else {
            text = text.substr(2);
            if (!text.empty() && (text.front() == '/' || text.front() == '!')) text = text.substr(1);
            text = Trim(text);
        }
        if (!out.empty()) out += '\n';
        out += text;
    }
    return Trim(out);
}
