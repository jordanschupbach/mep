// Everything mep's C++ language server answers that is not a
// diagnostic: completion, hover, document symbols, folding, go to
// definition, references, document highlight, rename and signature help.
//
// All of them read the same index (cpp_lsp_index.h) and share one
// principle: **an answer this server is not sure of is not an answer**.
// A completion list that invents members, a hover that guesses a type or
// a rename that misses an occurrence are each worse than nothing,
// because the person believes them. So where the scope model runs out --
// a member of a type from a header nobody read, a name a macro invented
// -- these functions return empty rather than plausible.
//
// The one place that is relaxed is completion, which may also offer the
// standard library's names from the built-in vocabulary (cpp_lsp_vocab.cpp)
// and the keywords: offering a name that turns out not to fit the context
// costs a keystroke, and a completion list with nothing in it costs the
// feature.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpp_ast.h"
#include "cpp_lsp.h"
#include "cpp_lsp_index.h"

namespace {

CppLspOptions PositionOptions() {
    // The position-taking features are pure by default: no filesystem,
    // no include resolution. The two that need it (completion inside an
    // `#include`, and go to definition across one) take an options
    // argument of their own.
    CppLspOptions opts;
    opts.check_files = false;
    return opts;
}

// --- Symbol kinds -----------------------------------------------------

CppLspKind CompletionKind(CppSymbolKind kind) {
    switch (kind) {
        case CppSymbolKind::Namespace: return CppLspKind::Module;
        case CppSymbolKind::Class: return CppLspKind::Class;
        case CppSymbolKind::Struct: return CppLspKind::Struct;
        case CppSymbolKind::Union: return CppLspKind::Struct;
        case CppSymbolKind::Enum: return CppLspKind::Enum;
        case CppSymbolKind::Enumerator: return CppLspKind::EnumMember;
        case CppSymbolKind::Function: return CppLspKind::Function;
        case CppSymbolKind::Method: return CppLspKind::Method;
        case CppSymbolKind::Constructor: return CppLspKind::Constructor;
        case CppSymbolKind::Destructor: return CppLspKind::Method;
        case CppSymbolKind::Variable: return CppLspKind::Variable;
        case CppSymbolKind::Field: return CppLspKind::Field;
        case CppSymbolKind::Parameter: return CppLspKind::Variable;
        case CppSymbolKind::Typedef: return CppLspKind::Class;
        case CppSymbolKind::Macro: return CppLspKind::Constant;
        case CppSymbolKind::Concept: return CppLspKind::TypeParameter;
        case CppSymbolKind::TemplateParam: return CppLspKind::TypeParameter;
        case CppSymbolKind::Label: return CppLspKind::Value;
    }
    return CppLspKind::Text;
}

// --- Token helpers ----------------------------------------------------

/** @brief The last token that ends at or before a position, skipping directive tokens. */
int TokenBefore(const CppIndex &index, int line, int col) {
    int best = -1;
    for (size_t i = 0; i < index.parse.raw_tokens.size(); i++) {
        const CppToken &token = index.parse.raw_tokens[i];
        if (token.kind == CppTokKind::End) break;
        if (token.end.line > line || (token.end.line == line && token.end.col > col)) break;
        best = static_cast<int>(i);
    }
    return best;
}

bool IsIncludeLine(const CppIndex &index, int line, const CppDirective **out) {
    for (const CppDirective &directive : index.parse.directives) {
        if (directive.kind != CppDirectiveKind::Include) continue;
        if (directive.line != line) continue;
        if (out != nullptr) *out = &directive;
        return true;
    }
    return false;
}

std::string LastComponentOf(const std::string &qualified) {
    const size_t at = qualified.rfind("::");
    return at == std::string::npos ? qualified : qualified.substr(at + 2);
}

/** @brief Adds `prefix`-matching entries of a vocabulary table to a completion list. */
void AddVocabItems(const std::vector<CppLspVocabEntry> &vocab, const std::string &prefix, CppLspKind kind,
                   int replace_start, int replace_end, std::vector<CppLspCompletionItem> *out) {
    for (const CppLspVocabEntry &entry : vocab) {
        const std::string name = entry.name;
        if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
        CppLspCompletionItem item;
        item.label = name;
        item.insert_text = name;
        item.kind = kind;
        item.detail = entry.detail;
        item.documentation = entry.doc;
        item.replace_start = replace_start;
        item.replace_end = replace_end;
        out->push_back(item);
    }
}

}  // namespace

// --- Document symbols -------------------------------------------------

namespace {

class SymbolCollector {
public:
    explicit SymbolCollector(const std::vector<std::string> &lines) : lines_(lines) {}

    std::vector<CppLspSymbol> Run(const CppNode *unit, const CppParseResult &parse) {
        for (const auto &entry : parse.macros) {
            const CppMacro &macro = entry.second;
            CppLspSymbol symbol;
            symbol.name = macro.name;
            symbol.kind = 14;  // Constant
            symbol.detail = macro.function_like ? "function-like macro" : "macro";
            symbol.line_start = macro.line;
            symbol.line_end = macro.line;
            symbol.sel_line = macro.line;
            symbol.sel_col_start = ColumnOf(macro.line, macro.name);
            symbol.sel_col_end = symbol.sel_col_start + static_cast<int>(macro.name.size());
            out_.push_back(symbol);
        }
        Walk(unit, -1);
        // Macros come out of a map, so they are not in document order
        // with the rest; one sort at the end puts the whole outline in
        // the order the file reads.
        std::stable_sort(out_.begin(), out_.end(), [](const CppLspSymbol &a, const CppLspSymbol &b) {
            if (a.parent != b.parent) return false;  // keep siblings' relative order
            return a.line_start < b.line_start;
        });
        return out_;
    }

private:
    const std::vector<std::string> &lines_;
    std::vector<CppLspSymbol> out_;

    int ColumnOf(int line, const std::string &name) const {
        if (line < 0 || static_cast<size_t>(line) >= lines_.size()) return 0;
        const size_t at = lines_[static_cast<size_t>(line)].find(name);
        return at == std::string::npos ? 0 : static_cast<int>(at);
    }

    int Add(const CppNode *node, const std::string &name, int kind, const std::string &detail, int parent) {
        CppLspSymbol symbol;
        symbol.name = name;
        symbol.kind = kind;
        symbol.detail = detail;
        symbol.line_start = node->start.line;
        symbol.line_end = node->end.line;
        symbol.sel_line = node->name_pos.line;
        symbol.sel_col_start = node->name_pos.col;
        symbol.sel_col_end = node->name_end.col > node->name_pos.col
                                 ? node->name_end.col
                                 : node->name_pos.col + static_cast<int>(name.size());
        symbol.parent = parent;
        out_.push_back(symbol);
        return static_cast<int>(out_.size()) - 1;
    }

    void Walk(const CppNode *node, int parent) {
        if (node == nullptr) return;
        for (const CppNodePtr &child : node->body) WalkOne(child.get(), parent);
    }

    void WalkOne(const CppNode *decl, int parent) {
        if (decl == nullptr) return;
        switch (decl->kind) {
            case CppNodeKind::Namespace: {
                const int index =
                    Add(decl, decl->name.empty() ? "(anonymous namespace)" : decl->name, 3, "namespace", parent);
                Walk(decl, index);
                break;
            }
            case CppNodeKind::LinkageSpec:
                Walk(decl, parent);
                break;
            case CppNodeKind::Class: {
                if (!decl->is_definition && decl->name.empty()) break;
                std::string detail = decl->str_value;
                if (!decl->bases.empty()) {
                    detail += " : ";
                    for (size_t i = 0; i < decl->bases.size(); i++) {
                        if (i > 0) detail += ", ";
                        detail += decl->bases[i]->name;
                    }
                }
                const int index = Add(decl, decl->name.empty() ? "(anonymous)" : decl->name,
                                      decl->str_value == "class" ? 5 : 23, detail, parent);
                Walk(decl, index);
                break;
            }
            case CppNodeKind::Enum: {
                const int index =
                    Add(decl, decl->name.empty() ? "(anonymous enum)" : decl->name, 10, decl->str_value, parent);
                for (const CppNodePtr &enumerator : decl->body) {
                    if (enumerator == nullptr) continue;
                    Add(enumerator.get(), enumerator->name, 22, "", index);
                }
                break;
            }
            case CppNodeKind::Function: {
                const std::string return_type = decl->kids.empty() ? "" : CppTypeText(decl->kids.front().get());
                std::string detail = CppSignatureText(decl);
                if (!return_type.empty()) detail += " -> " + return_type;
                const bool is_member =
                    parent >= 0 || (!decl->qualified.empty() && decl->qualified.find("::") != std::string::npos);
                Add(decl, decl->qualified.empty() ? decl->name : decl->qualified, is_member ? 6 : 12, detail,
                    parent);
                break;
            }
            case CppNodeKind::Variable:
            case CppNodeKind::Field: {
                if (decl->name.empty()) break;
                const std::string type = decl->kids.empty() ? "" : CppTypeText(decl->kids.front().get());
                const bool constant =
                    decl->is_constexpr ||
                    (!decl->kids.empty() && decl->kids.front() != nullptr && decl->kids.front()->is_const);
                Add(decl, decl->name, decl->kind == CppNodeKind::Field ? 8 : (constant ? 14 : 13), type, parent);
                break;
            }
            case CppNodeKind::Typedef:
            case CppNodeKind::UsingAlias: {
                if (decl->name.empty()) break;
                Add(decl, decl->name, 5, decl->kids.empty() ? "" : CppTypeText(decl->kids.front().get()), parent);
                break;
            }
            case CppNodeKind::Concept:
                Add(decl, decl->name, 11, "concept", parent);
                break;
            case CppNodeKind::DeclStmt:
            case CppNodeKind::Friend:
                // One statement that declared several things, or a
                // `friend` wrapping one: each reads as its own entry.
                for (const CppNodePtr &kid : decl->kids) WalkOne(kid.get(), parent);
                break;
            default:
                break;
        }
    }
};

}  // namespace

std::vector<CppLspSymbol> CppLspSymbols(const std::vector<std::string> &lines) {
    const CppIndex index = BuildCppIndex(lines, PositionOptions());
    SymbolCollector collector(lines);
    return collector.Run(index.parse.unit.get(), index.parse);
}

// --- Folding ----------------------------------------------------------

std::vector<CppLspFold> CppLspFolds(const std::vector<std::string> &lines) {
    const CppIndex index = BuildCppIndex(lines, PositionOptions());
    std::vector<CppLspFold> folds;

    // Braced regions, from the tokens rather than the tree: a fold is
    // wanted for every `{ ... }` that spans lines, including the ones
    // inside expressions and the ones the parser recovered from.
    std::vector<CppPos> open_braces;
    for (const CppToken &token : index.parse.raw_tokens) {
        if (token.kind != CppTokKind::Op || token.directive_line >= 0) continue;
        if (token.text == "{") {
            open_braces.push_back(token.start);
            continue;
        }
        if (token.text != "}" || open_braces.empty()) continue;
        const CppPos open = open_braces.back();
        open_braces.pop_back();
        if (token.start.line > open.line) {
            CppLspFold fold;
            fold.start_line = open.line;
            fold.end_line = token.start.line;
            folds.push_back(fold);
        }
    }

    // Conditional groups: `#if` to its `#endif`.
    for (const CppDirective &directive : index.parse.directives) {
        if (directive.end_directive_line <= directive.line) continue;
        CppLspFold fold;
        fold.start_line = directive.line;
        fold.end_line = directive.end_directive_line;
        fold.kind = "region";
        folds.push_back(fold);
    }

    // Comment runs, and any block comment that spans lines.
    std::vector<const CppComment *> own_line;
    for (const CppComment &comment : index.parse.comments) {
        if (comment.block && comment.end_line > comment.line) {
            CppLspFold fold;
            fold.start_line = comment.line;
            fold.end_line = comment.end_line;
            fold.kind = "comment";
            folds.push_back(fold);
            continue;
        }
        if (comment.own_line && !comment.block) own_line.push_back(&comment);
    }
    size_t run_start = 0;
    while (run_start < own_line.size()) {
        size_t run_end = run_start;
        while (run_end + 1 < own_line.size() && own_line[run_end + 1]->line == own_line[run_end]->line + 1) run_end++;
        if (run_end > run_start) {
            CppLspFold fold;
            fold.start_line = own_line[run_start]->line;
            fold.end_line = own_line[run_end]->line;
            fold.kind = "comment";
            folds.push_back(fold);
        }
        run_start = run_end + 1;
    }

    // The include block at the top of the file.
    int include_start = -1;
    int include_end = -1;
    for (const CppInclude &include : index.parse.includes) {
        if (include_start < 0) {
            include_start = include.line;
            include_end = include.line;
            continue;
        }
        if (include.line <= include_end + 2) {
            include_end = include.line;
            continue;
        }
        break;
    }
    if (include_start >= 0 && include_end > include_start) {
        CppLspFold fold;
        fold.start_line = include_start;
        fold.end_line = include_end;
        fold.kind = "imports";
        folds.push_back(fold);
    }

    std::stable_sort(folds.begin(), folds.end(), [](const CppLspFold &a, const CppLspFold &b) {
        if (a.start_line != b.start_line) return a.start_line < b.start_line;
        return a.end_line > b.end_line;
    });
    return folds;
}

// --- Hover ------------------------------------------------------------

namespace {

std::string DescribeSymbol(const CppIndex &index, const CppSymbol &symbol) {
    std::string out = CppSymbolDetail(symbol);
    if (!symbol.qualified.empty() && symbol.qualified != symbol.name &&
        out.find(symbol.qualified) == std::string::npos) {
        out += "\n" + symbol.qualified;
    }
    if (symbol.external) {
        out += "\nfrom " + std::filesystem::path(symbol.file).filename().string();
    } else if (symbol.pos.line >= 0) {
        out += "\ndeclared on line " + std::to_string(symbol.pos.line + 1);
    }
    std::string doc = symbol.doc;
    if (doc.empty() && !symbol.external) doc = CppDocCommentAbove(index, symbol.range_start.line);
    if (!doc.empty()) out += "\n\n" + doc;
    return out;
}

}  // namespace

CppLspHoverInfo CppLspHover(const std::vector<std::string> &lines, int line, int col, const CppLspOptions &opts) {
    CppLspHoverInfo info;
    const CppIndex index = BuildCppIndex(lines, opts);
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return info;

    // An `#include` line answers with the header it names, and with the
    // file it resolved to when there is one.
    const CppDirective *directive = nullptr;
    if (IsIncludeLine(index, line, &directive) && directive != nullptr) {
        std::string text = (directive->angled ? "#include <" : "#include \"") + directive->header +
                           (directive->angled ? ">" : "\"");
        for (const CppInclude &include : index.parse.includes) {
            if (include.line != line) continue;
            if (!include.resolved.empty()) text += "\n" + include.resolved;
        }
        for (const CppLspVocabEntry &entry : CppLspHeaderVocab()) {
            if (directive->header == entry.name) {
                text += "\n\n";
                text += entry.doc;
            }
        }
        info.found = true;
        info.text = text;
        info.line = line;
        info.col_start = directive->start.col;
        info.col_end = static_cast<int>(lines[static_cast<size_t>(line)].size());
        return info;
    }

    int start = 0;
    int end = 0;
    const std::string word = CppWordAt(lines, line, col, &start, &end);
    if (word.empty()) return info;
    if (CppPositionInLiteral(index, line, col)) return info;

    info.line = line;
    info.col_start = start;
    info.col_end = end;

    const CppOccurrence *occurrence = CppOccurrenceAt(index, line, start);
    if (occurrence != nullptr && occurrence->symbol >= 0) {
        info.found = true;
        info.text = DescribeSymbol(index, index.symbols[static_cast<size_t>(occurrence->symbol)]);
        return info;
    }
    // A macro invocation is not in the occurrence list -- its tokens were
    // replaced before the parser saw them -- so it is answered from the
    // macro table directly.
    const auto macro = index.parse.macros.find(word);
    if (macro != index.parse.macros.end()) {
        std::string text = "#define " + macro->second.name;
        if (macro->second.function_like) {
            text += "(";
            for (size_t i = 0; i < macro->second.params.size(); i++) {
                if (i > 0) text += ", ";
                text += macro->second.params[i];
            }
            text += ")";
        }
        std::string body;
        for (const CppToken &token : macro->second.body) {
            if (!body.empty() && token.space_before) body += ' ';
            body += token.text;
        }
        if (!body.empty()) text += " " + body;
        text += "\ndefined on line " + std::to_string(macro->second.line + 1);
        const std::string doc = CppDocCommentAbove(index, macro->second.line);
        if (!doc.empty()) text += "\n\n" + doc;
        info.found = true;
        info.text = text;
        return info;
    }
    for (const CppLspVocabEntry &entry : CppLspKeywordVocab()) {
        if (word != entry.name) continue;
        info.found = true;
        info.text = std::string(entry.detail) + "\n\n" + entry.doc;
        return info;
    }
    for (const CppLspVocabEntry &entry : CppLspStdVocab()) {
        if (word != entry.name) continue;
        info.found = true;
        info.text = std::string("std::") + entry.name + "\n" + entry.detail + "\n\n" + entry.doc;
        return info;
    }
    return info;
}

// --- Definition -------------------------------------------------------

CppLspLocation CppLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                const CppLspOptions &opts) {
    CppLspLocation location;
    const CppIndex index = BuildCppIndex(lines, opts);
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return location;

    // `#include "editor.h"` jumps into the header.
    for (const CppInclude &include : index.parse.includes) {
        if (include.line != line || include.resolved.empty()) continue;
        location.found = true;
        location.path = include.resolved;
        location.line = 0;
        location.col = 0;
        return location;
    }

    int start = 0;
    int end = 0;
    const std::string word = CppWordAt(lines, line, col, &start, &end);
    if (word.empty() || CppPositionInLiteral(index, line, col)) return location;

    const CppOccurrence *occurrence = CppOccurrenceAt(index, line, start);
    if (occurrence != nullptr && occurrence->symbol >= 0) {
        const CppSymbol &symbol = index.symbols[static_cast<size_t>(occurrence->symbol)];
        // Prefer a definition over a declaration: clicking a member
        // function's name should land on its body, wherever in this file
        // that is.
        const CppSymbol *target = &symbol;
        if (!symbol.is_definition) {
            for (const CppSymbol &candidate : index.symbols) {
                if (!candidate.is_definition || candidate.name != symbol.name) continue;
                if (candidate.kind != symbol.kind) continue;
                if (!symbol.qualified.empty() && !candidate.qualified.empty() &&
                    LastComponentOf(candidate.qualified) != LastComponentOf(symbol.qualified)) {
                    continue;
                }
                target = &candidate;
                break;
            }
        }
        location.found = true;
        location.path = target->external ? target->file : "";
        location.line = target->pos.line;
        location.col = target->pos.col;
        return location;
    }
    const auto macro = index.parse.macros.find(word);
    if (macro != index.parse.macros.end()) {
        location.found = true;
        location.line = macro->second.line;
        location.col = 0;
        const std::string &text = lines[static_cast<size_t>(std::min<size_t>(
            static_cast<size_t>(macro->second.line), lines.size() - 1))];
        const size_t at = text.find(word);
        if (at != std::string::npos) location.col = static_cast<int>(at);
        return location;
    }
    return location;
}

// --- References, highlight and rename ---------------------------------

CppLspReferenceSet CppLspReferences(const std::vector<std::string> &lines, int line, int col) {
    CppLspReferenceSet result;
    const CppIndex index = BuildCppIndex(lines, PositionOptions());
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return result;
    int start = 0;
    int end = 0;
    const std::string word = CppWordAt(lines, line, col, &start, &end);
    if (word.empty() || CppPositionInLiteral(index, line, col)) return result;
    result.name = word;

    const CppOccurrence *occurrence = CppOccurrenceAt(index, line, start);
    if (occurrence == nullptr || occurrence->symbol < 0) {
        // A macro: every occurrence of the name is one, since the
        // preprocessor replaced them all.
        const auto macro = index.parse.macros.find(word);
        if (macro == index.parse.macros.end()) return result;
        for (const CppToken &token : index.parse.raw_tokens) {
            if (token.kind != CppTokKind::Ident || token.text != word) continue;
            CppLspReference reference;
            reference.line = token.start.line;
            reference.col_start = token.start.col;
            reference.col_end = token.start.col + static_cast<int>(word.size());
            reference.is_write = token.start.line == macro->second.line;
            result.refs.push_back(reference);
        }
        result.found = !result.refs.empty();
        return result;
    }

    const CppSymbol &symbol = index.symbols[static_cast<size_t>(occurrence->symbol)];
    for (const CppOccurrence &other : index.occurrences) {
        if (other.symbol != occurrence->symbol) continue;
        CppLspReference reference;
        reference.line = other.pos.line;
        reference.col_start = other.pos.col;
        reference.col_end = other.pos.col + other.length;
        reference.is_write = other.is_write || other.is_declaration;
        result.refs.push_back(reference);
    }
    std::stable_sort(result.refs.begin(), result.refs.end(),
                     [](const CppLspReference &a, const CppLspReference &b) {
                         if (a.line != b.line) return a.line < b.line;
                         return a.col_start < b.col_start;
                     });
    result.found = !result.refs.empty();
    if (symbol.external) {
        result.rename_blocked_reason =
            "'" + word + "' is declared in " + std::filesystem::path(symbol.file).filename().string() +
            ", which this server will not edit";
    } else if (IsCppKeyword(word)) {
        result.rename_blocked_reason = "'" + word + "' is a keyword";
    } else if (symbol.kind == CppSymbolKind::Method || symbol.kind == CppSymbolKind::Field) {
        // A member may be overridden or used in another translation
        // unit; renaming it here would rename half of it.
        const CppScope &scope = index.scopes[static_cast<size_t>(symbol.scope)];
        if (!scope.unresolved_bases.empty()) {
            result.rename_blocked_reason =
                "'" + word + "' belongs to a class whose base classes this server could not read";
        }
    }
    return result;
}

// --- Signature help ---------------------------------------------------

namespace {

/** @brief Splits a signature's parameter list into the individual labels. */
std::vector<std::string> SplitParameters(const std::string &label) {
    const size_t open = label.find('(');
    if (open == std::string::npos) return {};
    int depth = 0;
    std::vector<std::string> params;
    std::string current;
    for (size_t i = open; i < label.size(); i++) {
        const char c = label[i];
        if (c == '(' || c == '[' || c == '<') depth++;
        if (c == ')' || c == ']' || c == '>') {
            depth--;
            if (depth == 0) break;
        }
        if (depth == 1 && c == ',') {
            params.push_back(current);
            current.clear();
            continue;
        }
        if (!(depth == 1 && current.empty() && (c == ' ' || c == '('))) current += c;
    }
    if (!current.empty()) params.push_back(current);
    for (std::string &param : params) {
        while (!param.empty() && param.front() == ' ') param.erase(param.begin());
        while (!param.empty() && param.back() == ' ') param.pop_back();
    }
    return params;
}

}  // namespace

CppLspSignature CppLspSignatureHelp(const std::vector<std::string> &lines, int line, int col) {
    CppLspSignature signature;
    const CppIndex index = BuildCppIndex(lines, PositionOptions());
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return signature;
    if (CppPositionInLiteral(index, line, col)) return signature;

    // Walk back through the tokens for the `(` that is still open, and
    // count the commas at its level on the way.
    const int from = TokenBefore(index, line, col);
    if (from < 0) return signature;
    int depth = 0;
    int active = 0;
    int open_token = -1;
    for (int i = from; i >= 0; i--) {
        const CppToken &token = index.parse.raw_tokens[static_cast<size_t>(i)];
        if (token.directive_line >= 0 || token.kind != CppTokKind::Op) continue;
        if (token.text == ")" || token.text == "]" || token.text == "}") {
            depth++;
            continue;
        }
        if (token.text == "[" || token.text == "{") {
            if (depth == 0) return signature;  // a braced list, not a call
            depth--;
            continue;
        }
        if (token.text == "(") {
            if (depth == 0) {
                open_token = i;
                break;
            }
            depth--;
            continue;
        }
        if (token.text == ";") return signature;
        if (token.text == "," && depth == 0) active++;
    }
    if (open_token <= 0) return signature;
    const CppToken &callee = index.parse.raw_tokens[static_cast<size_t>(open_token - 1)];
    if (callee.kind != CppTokKind::Ident) return signature;

    signature.active_param = active;
    const int scope = CppScopeAt(index, callee.start);
    const int symbol_index = CppLookupName(index, scope, callee.text);
    if (symbol_index >= 0) {
        const CppSymbol &symbol = index.symbols[static_cast<size_t>(symbol_index)];
        if (symbol.kind == CppSymbolKind::Function || symbol.kind == CppSymbolKind::Method ||
            symbol.kind == CppSymbolKind::Constructor || symbol.kind == CppSymbolKind::Macro) {
            signature.found = true;
            signature.label = symbol.detail.empty() ? symbol.name : symbol.detail;
            signature.documentation = symbol.doc;
            signature.params = SplitParameters(signature.label);
            return signature;
        }
        // A variable of a class type, called through `operator()`, and
        // anything else this server cannot describe: say nothing rather
        // than showing the variable's own declaration as a signature.
        if (symbol.kind == CppSymbolKind::Class || symbol.kind == CppSymbolKind::Struct) {
            const int class_scope = symbol.child_scope;
            const int constructor = class_scope >= 0 ? CppLookupMember(index, class_scope, symbol.name) : -1;
            if (constructor >= 0) {
                const CppSymbol &target = index.symbols[static_cast<size_t>(constructor)];
                signature.found = true;
                signature.label = target.detail.empty() ? target.name : target.detail;
                signature.documentation = target.doc;
                signature.params = SplitParameters(signature.label);
                return signature;
            }
        }
        return signature;
    }
    for (const CppLspVocabEntry &entry : CppLspStdVocab()) {
        if (callee.text != entry.name) continue;
        signature.found = true;
        signature.label = entry.detail;
        signature.documentation = entry.doc;
        signature.params = SplitParameters(signature.label);
        return signature;
    }
    return signature;
}

// --- Completion -------------------------------------------------------

namespace {

/** @brief What the cursor is completing: the context decides the whole list. */
enum class CompletionContext {
    Expression,   // an ordinary name
    Member,       // after `.` or `->`
    Qualified,    // after `::`
    Directive,    // after `#`
    IncludePath,  // inside `#include <...>` or `#include "..."`
};

struct CompletionRequest {
    CompletionContext context = CompletionContext::Expression;
    std::string prefix;
    std::string qualifier;  // the text before `::`, `.` or `->`
    int replace_start = 0;
    int replace_end = 0;
    bool angled_include = false;
};

/** @brief Reads the line to decide what is being completed. */
CompletionRequest ReadRequest(const std::vector<std::string> &lines, int line, int col) {
    CompletionRequest request;
    const std::string &text = lines[static_cast<size_t>(line)];
    const size_t cursor = std::min(static_cast<size_t>(std::max(0, col)), text.size());
    size_t start = cursor;
    while (start > 0 && IsCppIdentChar(static_cast<unsigned char>(text[start - 1]))) start--;
    request.prefix = text.substr(start, cursor - start);
    request.replace_start = static_cast<int>(start);
    request.replace_end = static_cast<int>(cursor);

    // A directive line is decided by the first non-space character.
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) first++;
    if (first < text.size() && text[first] == '#') {
        const size_t word_start = first + 1;
        size_t word_end = word_start;
        while (word_end < text.size() && IsCppIdentChar(static_cast<unsigned char>(text[word_end]))) word_end++;
        const std::string directive = text.substr(word_start, word_end - word_start);
        if (directive == "include" || directive == "include_next" || directive == "import") {
            const size_t open = text.find_first_of("<\"", word_end);
            if (open != std::string::npos && open < cursor) {
                request.context = CompletionContext::IncludePath;
                request.angled_include = text[open] == '<';
                request.prefix = text.substr(open + 1, cursor - open - 1);
                request.replace_start = static_cast<int>(open + 1);
                request.replace_end = static_cast<int>(cursor);
                return request;
            }
        }
        if (cursor <= word_end) {
            request.context = CompletionContext::Directive;
            request.prefix = text.substr(word_start, cursor - word_start);
            request.replace_start = static_cast<int>(word_start);
            request.replace_end = static_cast<int>(cursor);
            return request;
        }
    }

    // `a.b`, `a->b` and `A::b` each name where to look.
    size_t before = start;
    while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t')) before--;
    if (before >= 2 && text.compare(before - 2, 2, "::") == 0) {
        request.context = CompletionContext::Qualified;
        size_t qualifier_end = before - 2;
        size_t qualifier_start = qualifier_end;
        while (qualifier_start > 0 &&
               (IsCppIdentChar(static_cast<unsigned char>(text[qualifier_start - 1])) ||
                (qualifier_start >= 2 && text.compare(qualifier_start - 2, 2, "::") == 0))) {
            if (IsCppIdentChar(static_cast<unsigned char>(text[qualifier_start - 1]))) {
                qualifier_start--;
                continue;
            }
            qualifier_start -= 2;
        }
        request.qualifier = text.substr(qualifier_start, qualifier_end - qualifier_start);
        return request;
    }
    if (before >= 2 && text.compare(before - 2, 2, "->") == 0) {
        request.context = CompletionContext::Member;
        size_t object_end = before - 2;
        size_t object_start = object_end;
        while (object_start > 0 && IsCppIdentChar(static_cast<unsigned char>(text[object_start - 1]))) object_start--;
        request.qualifier = text.substr(object_start, object_end - object_start);
        return request;
    }
    if (before >= 1 && text[before - 1] == '.' && !(before >= 2 && text[before - 2] == '.')) {
        request.context = CompletionContext::Member;
        size_t object_end = before - 1;
        size_t object_start = object_end;
        while (object_start > 0 && IsCppIdentChar(static_cast<unsigned char>(text[object_start - 1]))) object_start--;
        request.qualifier = text.substr(object_start, object_end - object_start);
        return request;
    }
    return request;
}

void AddSymbolItem(const CppIndex &index, const CppSymbol &symbol, const CompletionRequest &request,
                   std::vector<CppLspCompletionItem> *out) {
    if (symbol.name.empty()) return;
    if (!request.prefix.empty() && symbol.name.compare(0, request.prefix.size(), request.prefix) != 0) return;
    CppLspCompletionItem item;
    item.label = symbol.name;
    item.insert_text = symbol.name;
    item.kind = CompletionKind(symbol.kind);
    item.detail = CppSymbolDetail(symbol);
    item.documentation = symbol.doc.empty() ? CppDocCommentAbove(index, symbol.range_start.line) : symbol.doc;
    item.replace_start = request.replace_start;
    item.replace_end = request.replace_end;
    out->push_back(item);
}

}  // namespace

std::vector<CppLspCompletionItem> CppLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const CppLspOptions &opts) {
    std::vector<CppLspCompletionItem> items;
    if (line < 0 || static_cast<size_t>(line) >= lines.size()) return items;
    const CppIndex index = BuildCppIndex(lines, opts);
    const CompletionRequest request = ReadRequest(lines, line, col);
    if (request.context != CompletionContext::IncludePath && CppPositionInLiteral(index, line, col)) return items;

    switch (request.context) {
        case CompletionContext::Directive:
            AddVocabItems(CppLspDirectiveVocab(), request.prefix, CppLspKind::Keyword, request.replace_start,
                          request.replace_end, &items);
            return items;

        case CompletionContext::IncludePath: {
            if (request.angled_include) {
                AddVocabItems(CppLspHeaderVocab(), request.prefix, CppLspKind::File, request.replace_start,
                              request.replace_end, &items);
                return items;
            }
            // A quoted include completes against the files next to the
            // document, which is the only place a quoted include of this
            // server's making can resolve to.
            if (!opts.check_files || opts.doc_dir.empty()) return items;
            std::error_code ec;
            std::vector<std::string> roots{opts.doc_dir};
            for (const std::string &dir : opts.include_dirs) roots.push_back(dir);
            for (const std::string &root : roots) {
                std::filesystem::directory_iterator it(root, ec);
                if (ec) continue;
                for (const std::filesystem::directory_entry &entry : it) {
                    const std::string name = entry.path().filename().string();
                    const bool header = name.size() > 2 &&
                                        (name.rfind(".h") == name.size() - 2 ||
                                         name.rfind(".hpp") == name.size() - 4 ||
                                         name.rfind(".hh") == name.size() - 3);
                    if (!entry.is_regular_file(ec) || !header) continue;
                    if (!request.prefix.empty() && name.compare(0, request.prefix.size(), request.prefix) != 0) {
                        continue;
                    }
                    CppLspCompletionItem item;
                    item.label = name;
                    item.insert_text = name;
                    item.kind = CppLspKind::File;
                    item.detail = root == opts.doc_dir ? "next to this file" : root;
                    item.replace_start = request.replace_start;
                    item.replace_end = request.replace_end;
                    items.push_back(item);
                }
            }
            return items;
        }

        case CompletionContext::Member: {
            const int scope = CppScopeAt(index, CppPos{line, request.replace_start});
            const int object = CppLookupName(index, scope, request.qualifier);
            if (object < 0) return items;
            const CppSymbol &symbol = index.symbols[static_cast<size_t>(object)];
            // A standard type answers from the vocabulary; anything
            // declared in this file (or a header it includes) answers
            // from its own class scope.
            const std::vector<CppLspVocabEntry> *members = CppLspTypeMembers(symbol.type_base);
            if (members != nullptr) {
                AddVocabItems(*members, request.prefix, CppLspKind::Method, request.replace_start,
                              request.replace_end, &items);
                return items;
            }
            const int type_symbol = CppResolveTypeName(index, scope, symbol.type_base);
            if (type_symbol < 0) return items;
            const int class_scope = index.symbols[static_cast<size_t>(type_symbol)].child_scope;
            if (class_scope < 0) return items;
            std::vector<int> visit{class_scope};
            std::set<int> seen;
            while (!visit.empty()) {
                const int current = visit.back();
                visit.pop_back();
                if (!seen.insert(current).second) continue;
                for (int member : index.scopes[static_cast<size_t>(current)].symbols) {
                    AddSymbolItem(index, index.symbols[static_cast<size_t>(member)], request, &items);
                }
                for (int base : index.scopes[static_cast<size_t>(current)].base_scopes) visit.push_back(base);
            }
            return items;
        }

        case CompletionContext::Qualified: {
            if (request.qualifier == "std") {
                AddVocabItems(CppLspStdVocab(), request.prefix, CppLspKind::Function, request.replace_start,
                              request.replace_end, &items);
                return items;
            }
            const int scope = CppScopeAt(index, CppPos{line, request.replace_start});
            const int owner = CppLookupName(index, scope, LastComponentOf(request.qualifier));
            if (owner < 0) return items;
            const int inner = index.symbols[static_cast<size_t>(owner)].child_scope;
            if (inner < 0) return items;
            std::vector<int> visit{inner};
            std::set<int> seen;
            while (!visit.empty()) {
                const int current = visit.back();
                visit.pop_back();
                if (!seen.insert(current).second) continue;
                for (int member : index.scopes[static_cast<size_t>(current)].symbols) {
                    AddSymbolItem(index, index.symbols[static_cast<size_t>(member)], request, &items);
                }
                for (int base : index.scopes[static_cast<size_t>(current)].base_scopes) visit.push_back(base);
            }
            return items;
        }

        case CompletionContext::Expression:
        default:
            break;
    }

    // An ordinary name: everything visible from here, nearest scope
    // first, then the file's macros, then the language and the standard
    // library.
    std::set<std::string> offered;
    int scope = CppScopeAt(index, CppPos{line, request.replace_start});
    int guard = 0;
    while (scope >= 0 && guard++ < 100) {
        const CppScope &here = index.scopes[static_cast<size_t>(scope)];
        std::vector<int> visit{scope};
        std::set<int> seen;
        while (!visit.empty()) {
            const int current = visit.back();
            visit.pop_back();
            if (!seen.insert(current).second) continue;
            for (int member : index.scopes[static_cast<size_t>(current)].symbols) {
                const CppSymbol &symbol = index.symbols[static_cast<size_t>(member)];
                if (!offered.insert(symbol.name).second) continue;
                AddSymbolItem(index, symbol, request, &items);
            }
            for (int base : index.scopes[static_cast<size_t>(current)].base_scopes) visit.push_back(base);
        }
        scope = here.parent;
    }
    AddVocabItems(CppLspKeywordVocab(), request.prefix, CppLspKind::Keyword, request.replace_start,
                  request.replace_end, &items);
    AddVocabItems(CppLspStdVocab(), request.prefix, CppLspKind::Function, request.replace_start,
                  request.replace_end, &items);
    return items;
}
