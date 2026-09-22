// The diagnostics half of mep's own C++ language server: what it is
// willing to say about a file it can see but cannot compile.
//
// The rule every check here obeys, and the reason the list is as short
// as it is: **report only what a careful reader could confirm from this
// file**. A C++ linter without a compiler is one bad heuristic away from
// being switched off, and a linter that has been switched off reports
// nothing at all -- so a check that cannot be sure says nothing, and
// several of them switch themselves off entirely when the file turns out
// to include a header this server could not read.
//
// The checks, grouped by what makes them safe:
//
//   - **Lexical and preprocessor** (unterminated string, stray `#endif`,
//     a macro redefined with a different body): the evidence is the
//     tokens themselves.
//   - **Binding hygiene** (a local assigned and never read, a static
//     function nobody calls, a declaration shadowing another): the
//     evidence is the scope model, and each of these is gated on the
//     declaration being one this file entirely owns.
//   - **Local idioms** (an assignment inside an `if`, an empty `if`
//     body, a self-assignment, a duplicated condition, code after a
//     `return`): the evidence is one statement and its neighbours.
//   - **Class-shaped mistakes** (a member initializer list out of order,
//     virtual functions with a public non-virtual destructor): the
//     evidence is the class definition right there in the file.
//   - **Name checks** (a member a fully-known class does not have, a call
//     with the wrong number of arguments, an undefined name): each one
//     names the exact conditions under which it dares speak, below.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpp_ast.h"
#include "cpp_lsp.h"
#include "cpp_lsp_index.h"

namespace {

/** @brief Maps a parser/preprocessor error code to the severity it deserves. */
CppLspSeverity SeverityForParseCode(const std::string &code) {
    if (code == "pp-macro-redefined" || code == "pp-warning" || code == "pp-unknown-directive") {
        return CppLspSeverity::Warning;
    }
    return CppLspSeverity::Error;
}

/**
 * @brief Renders an expression back to a canonical string, for the checks that compare two of them.
 *
 * Only the shapes a reader would call "the same thing written twice"
 * produce a non-empty result: names, member accesses, `this`, literals
 * and constant subscripts. Anything with a call in it renders empty, so
 * `f() == f()` is never claimed to be a duplicate of itself.
 */
std::string RenderExpression(const CppNode *node) {
    if (node == nullptr) return "";
    switch (node->kind) {
        case CppNodeKind::Id:
            return node->str_value.empty() ? node->name : node->str_value;
        case CppNodeKind::This:
            return "this";
        case CppNodeKind::Literal:
            return node->name;
        case CppNodeKind::Member: {
            const std::string object = RenderExpression(node->kids.empty() ? nullptr : node->kids.front().get());
            if (object.empty()) return "";
            return object + node->str_value + node->name;
        }
        case CppNodeKind::Paren:
            return RenderExpression(node->kids.empty() ? nullptr : node->kids.front().get());
        case CppNodeKind::Subscript: {
            if (node->kids.size() < 2) return "";
            const std::string object = RenderExpression(node->kids[0].get());
            const std::string index = RenderExpression(node->kids[1].get());
            if (object.empty() || index.empty()) return "";
            return object + "[" + index + "]";
        }
        case CppNodeKind::Unary: {
            if (node->kids.empty()) return "";
            if (node->name == "++" || node->name == "--") return "";
            const std::string operand = RenderExpression(node->kids.front().get());
            if (operand.empty()) return "";
            return node->str_value == "postfix" ? operand + node->name : node->name + operand;
        }
        case CppNodeKind::Binary: {
            if (node->kids.size() < 2) return "";
            const std::string left = RenderExpression(node->kids[0].get());
            const std::string right = RenderExpression(node->kids[1].get());
            if (left.empty() || right.empty()) return "";
            return left + " " + node->name + " " + right;
        }
        default:
            return "";
    }
}

/** @brief Reports whether an expression contains a call, a `new` or anything else that can have an effect. */
bool HasSideEffect(const CppNode *node) {
    if (node == nullptr) return false;
    switch (node->kind) {
        case CppNodeKind::Call:
        case CppNodeKind::New:
        case CppNodeKind::Delete:
        case CppNodeKind::Throw:
        case CppNodeKind::Lambda:
            return true;
        case CppNodeKind::Unary:
            if (node->name == "++" || node->name == "--") return true;
            break;
        case CppNodeKind::Binary:
            if (node->name == "=" || (node->name.size() > 1 && node->name.back() == '=' && node->name != "==" &&
                                      node->name != "!=" && node->name != "<=" && node->name != ">=")) {
                return true;
            }
            break;
        default:
            break;
    }
    for (const CppNodePtr &kid : node->kids) {
        if (HasSideEffect(kid.get())) return true;
    }
    return false;
}

/** @brief The edit distance between two names, capped -- used only for "did you mean". */
int EditDistance(const std::string &a, const std::string &b, int cap) {
    if (std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size())) > cap) return cap + 1;
    std::vector<int> previous(b.size() + 1);
    std::vector<int> current(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) previous[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        current[0] = static_cast<int>(i);
        int best = current[0];
        for (size_t j = 1; j <= b.size(); j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            best = std::min(best, current[j]);
        }
        if (best > cap) return cap + 1;
        previous = current;
    }
    return previous[b.size()];
}

bool IsScalarType(const CppIndex &index, const CppSymbol &symbol) {
    if (symbol.ptr_depth > 0) return true;
    if (symbol.type_base.empty()) return false;
    if (IsCppFundamentalType(symbol.type_base)) return true;
    if (symbol.type_base == "size_t" || symbol.type_base == "ptrdiff_t") return true;
    const int type_symbol = CppResolveTypeName(index, symbol.scope, symbol.type_base);
    if (type_symbol < 0) return false;
    return index.symbols[static_cast<size_t>(type_symbol)].kind == CppSymbolKind::Enum;
}

// --- The collector ----------------------------------------------------

class DiagnosticCollector {
public:
    DiagnosticCollector(const CppIndex &index, const CppLspOptions &opts) : index_(index), opts_(opts) {}

    std::vector<CppLspDiagnostic> Run() {
        CountNames();
        ReportParseErrors();
        CheckIncludes();
        CheckIncludeGuard();
        CheckUsingNamespaceInHeader();
        CheckBindings();
        CheckShadowing();
        if (index_.parse.unit != nullptr) WalkDeclarations(index_.parse.unit->body);
        CheckMemberAccesses();
        CheckUndefinedNames();
        CheckNullLiteral();
        CheckLineLength();
        std::stable_sort(out_.begin(), out_.end(), [](const CppLspDiagnostic &a, const CppLspDiagnostic &b) {
            if (a.line != b.line) return a.line < b.line;
            return a.col_start < b.col_start;
        });
        return out_;
    }

private:
    const CppIndex &index_;
    const CppLspOptions &opts_;
    std::vector<CppLspDiagnostic> out_;
    std::unordered_map<std::string, int> name_counts_;
    // A file with a syntax error in it is exactly when an invented
    // "undefined name" or "no such member" is worst -- the tree is a
    // partial view of what the person is in the middle of typing.
    bool tree_is_sound_ = true;

    void Report(const std::string &code, const std::string &message, const CppPos &pos, int length,
                CppLspSeverity severity) {
        CppLspDiagnostic diagnostic;
        diagnostic.line = pos.line;
        diagnostic.col_start = pos.col;
        diagnostic.col_end = pos.col + std::max(1, length);
        diagnostic.severity = severity;
        diagnostic.code = code;
        diagnostic.message = message;
        out_.push_back(diagnostic);
    }

    void CountNames() {
        for (const CppSymbol &symbol : index_.symbols) name_counts_[symbol.name]++;
        for (const CppSyntaxError &error : index_.parse.errors) {
            if (error.code != "pp-macro-redefined" && error.code != "pp-warning" &&
                error.code != "pp-unknown-directive" && error.code != "pp-error") {
                tree_is_sound_ = false;
            }
        }
        if (index_.truncated) tree_is_sound_ = false;
    }

    // --- Parse, preprocessor and file-shaped checks --------------------

    void ReportParseErrors() {
        for (const CppSyntaxError &error : index_.parse.errors) {
            const int length = error.end.line == error.start.line ? std::max(1, error.end.col - error.start.col) : 1;
            Report(error.code, error.message, error.start, length, SeverityForParseCode(error.code));
        }
    }

    void CheckIncludes() {
        if (!opts_.check_files || opts_.doc_dir.empty()) return;
        // Only worth mentioning when another quoted include in the same
        // file *did* resolve: a file whose every quoted header is
        // somewhere this server cannot see is a file compiled with `-I`
        // flags nobody told it about, and that is this server's gap
        // rather than the file's.
        bool any_resolved = false;
        for (const CppInclude &include : index_.parse.includes) {
            if (!include.angled && !include.resolved.empty()) any_resolved = true;
        }
        if (!any_resolved) return;
        for (const CppInclude &include : index_.parse.includes) {
            if (include.angled || !include.resolved.empty()) continue;
            CppPos pos;
            pos.line = include.line;
            pos.col = include.path_col;
            // A Hint, not a warning, until the project has told this
            // server where its headers live: without `includeDirs` a
            // perfectly good `#include "lauxlib.h"` cannot be resolved,
            // and that is this server's gap rather than the file's.
            Report("include-not-found",
                   "Cannot find \"" + include.header + "\" next to this file" +
                       (opts_.include_dirs.empty() ? " (set `includeDirs` if it lives elsewhere)"
                                                   : " or in the configured include directories"),
                   pos, std::max(1, include.path_end - include.path_col),
                   opts_.include_dirs.empty() ? CppLspSeverity::Hint : CppLspSeverity::Warning);
        }
    }

    void CheckIncludeGuard() {
        if (!index_.file_is_header || index_.parse.unit == nullptr) return;
        if (index_.parse.unit->body.empty()) return;
        for (const CppDirective &directive : index_.parse.directives) {
            if (directive.kind == CppDirectiveKind::Pragma && directive.name == "once") return;
            // `#ifndef GUARD` / `#define GUARD` at the top, closed at the
            // bottom: the classic spelling.
            if (directive.kind == CppDirectiveKind::Ifndef && directive.depth == 0 &&
                directive.end_directive_line >= 0) {
                for (const CppDirective &define : index_.parse.directives) {
                    if (define.kind == CppDirectiveKind::Define && define.name == directive.name) return;
                }
            }
        }
        CppPos pos;
        Report("missing-include-guard",
               "This header has no include guard: add `#pragma once`, or an `#ifndef`/`#define` pair around it",
               pos, 1, CppLspSeverity::Hint);
    }

    void CheckUsingNamespaceInHeader() {
        if (!index_.file_is_header || index_.parse.unit == nullptr) return;
        for (const CppNodePtr &decl : index_.parse.unit->body) {
            if (decl == nullptr || decl->kind != CppNodeKind::UsingDirective) continue;
            Report("using-namespace-in-header",
                   "`using namespace " + decl->name +
                       ";` at file scope in a header leaks into every file that includes it",
                   decl->start, static_cast<int>(decl->name.size()) + 16, CppLspSeverity::Warning);
        }
    }

    // --- Binding hygiene ----------------------------------------------

    void CheckBindings() {
        for (size_t i = 0; i < index_.symbols.size(); i++) {
            const CppSymbol &symbol = index_.symbols[i];
            if (symbol.external || symbol.from_macro) continue;
            if (symbol.reads > 0 || symbol.address_taken) continue;
            if (symbol.name.empty() || symbol.name == "_") continue;
            if (HasMaybeUnusedAttribute(symbol)) continue;
            const CppScope::Kind scope_kind = index_.scopes[static_cast<size_t>(symbol.scope)].kind;
            if (symbol.kind == CppSymbolKind::Variable &&
                (scope_kind == CppScope::Kind::Function || scope_kind == CppScope::Kind::Block)) {
                // Only for a type whose destructor cannot be doing the
                // work: a `std::lock_guard` nobody reads is the point of
                // a `std::lock_guard`.
                if (!IsScalarType(index_, symbol)) continue;
                if (symbol.is_static || symbol.is_extern) continue;
                // A reference that is assigned *is* used: `for (bool &b :
                // flags) b = false;` writes through it, which is the
                // whole point of taking it by reference.
                if (symbol.is_ref && symbol.writes > 0) continue;
                Report("unused-variable",
                       "'" + symbol.name + "' is " + (symbol.writes > 0 ? "assigned" : "declared") +
                           " and never read",
                       symbol.pos, static_cast<int>(symbol.name.size()), CppLspSeverity::Warning);
                continue;
            }
            if (symbol.kind == CppSymbolKind::Function && symbol.is_definition && !index_.file_is_header) {
                if (!symbol.is_static && !IsInAnonymousNamespace(symbol)) continue;
                if (symbol.name == "main" || symbol.name.rfind("operator", 0) == 0) continue;
                if (symbol.is_template) continue;
                // An overload set resolves to one symbol, so the others
                // would look unused; the check steps aside for any
                // repeated name.
                if (name_counts_[symbol.name] > 1) continue;
                Report("unused-function", "'" + symbol.name + "' is defined here and never used in this file",
                       symbol.pos, static_cast<int>(symbol.name.size()), CppLspSeverity::Warning);
            }
        }
    }

    bool HasMaybeUnusedAttribute(const CppSymbol &symbol) const {
        if (symbol.node == nullptr) return false;
        for (const CppNodePtr &attribute : symbol.node->decorators) {
            if (attribute != nullptr && attribute->name.find("maybe_unused") != std::string::npos) return true;
        }
        return false;
    }

    bool IsInAnonymousNamespace(const CppSymbol &symbol) const {
        int scope = symbol.scope;
        int guard = 0;
        while (scope >= 0 && guard++ < 100) {
            const CppScope &here = index_.scopes[static_cast<size_t>(scope)];
            if (here.kind == CppScope::Kind::Namespace && here.name.empty()) return true;
            scope = here.parent;
        }
        return false;
    }

    void CheckShadowing() {
        for (const CppSymbol &symbol : index_.symbols) {
            if (symbol.kind != CppSymbolKind::Variable && symbol.kind != CppSymbolKind::Parameter) continue;
            if (symbol.external || symbol.name.empty() || symbol.name == "_") continue;
            const CppScope &scope = index_.scopes[static_cast<size_t>(symbol.scope)];
            if (scope.kind != CppScope::Kind::Block && scope.kind != CppScope::Kind::Function) continue;
            // A lambda's own parameters are not reported: GCC's -Wshadow
            // does not flag them either, and a callback whose parameter
            // is called what the surrounding value is called reads fine.
            if (scope.node != nullptr && scope.node->kind == CppNodeKind::Lambda) continue;
            const int shadowed = FindShadowedLocal(scope.parent, symbol.name, symbol.pos);
            if (shadowed < 0) continue;
            Report("shadowed-declaration",
                   "'" + symbol.name + "' shadows the one declared on line " +
                       std::to_string(index_.symbols[static_cast<size_t>(shadowed)].pos.line + 1),
                   symbol.pos, static_cast<int>(symbol.name.size()), CppLspSeverity::Warning);
        }
    }

    /**
     * @brief Finds a local or parameter of the same name in an enclosing block, or -1.
     *
     * Only locals shadowing locals: a member or a global with the same
     * name as a parameter is a house style in half the C++ ever written,
     * and reporting it is how a check gets switched off.
     */
    int FindShadowedLocal(int scope, const std::string &name, const CppPos &position) const {
        int current = scope;
        int guard = 0;
        while (current >= 0 && guard++ < 100) {
            const CppScope &here = index_.scopes[static_cast<size_t>(current)];
            if (here.kind != CppScope::Kind::Block && here.kind != CppScope::Kind::Function) return -1;
            for (int other : here.symbols) {
                const CppSymbol &candidate = index_.symbols[static_cast<size_t>(other)];
                if (candidate.name != name) continue;
                if (candidate.kind != CppSymbolKind::Variable && candidate.kind != CppSymbolKind::Parameter) {
                    continue;
                }
                // Only a declaration that is already in scope: a lambda
                // body sees the locals above it, not the ones the
                // enclosing function declares further down.
                if (!CppPosLess(candidate.pos, position)) continue;
                return other;
            }
            current = here.parent;
        }
        return -1;
    }

    // --- Statement and class checks -----------------------------------

    void WalkDeclarations(const std::vector<CppNodePtr> &body) {
        for (const CppNodePtr &decl : body) WalkDeclaration(decl.get());
    }

    void WalkDeclaration(const CppNode *node) {
        if (node == nullptr) return;
        switch (node->kind) {
            case CppNodeKind::Namespace:
            case CppNodeKind::LinkageSpec:
                WalkDeclarations(node->body);
                break;
            case CppNodeKind::Class:
                CheckClass(node);
                WalkDeclarations(node->body);
                break;
            case CppNodeKind::Function:
                CheckConstructorInitOrder(node);
                WalkStatements(node->body);
                for (const CppNodePtr &handler : node->handlers) WalkStatement(handler.get());
                break;
            case CppNodeKind::Variable:
            case CppNodeKind::Field:
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                break;
            case CppNodeKind::Friend:
            case CppNodeKind::DeclStmt:
                for (const CppNodePtr &kid : node->kids) WalkDeclaration(kid.get());
                break;
            default:
                break;
        }
    }

    void WalkStatements(const std::vector<CppNodePtr> &body) {
        CheckUnreachableCode(body);
        for (const CppNodePtr &stmt : body) WalkStatement(stmt.get());
    }

    void WalkStatement(const CppNode *node) {
        if (node == nullptr) return;
        switch (node->kind) {
            case CppNodeKind::Compound:
                WalkStatements(node->body);
                break;
            case CppNodeKind::If:
                CheckConditionAssignment(node);
                CheckEmptyBody(node);
                CheckDuplicateConditions(node);
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                WalkStatements(node->body);
                WalkStatements(node->orelse);
                break;
            case CppNodeKind::While:
            case CppNodeKind::DoWhile:
                CheckConditionAssignment(node);
                CheckEmptyBody(node);
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                WalkStatements(node->body);
                break;
            case CppNodeKind::For:
            case CppNodeKind::RangeFor:
            case CppNodeKind::Switch:
            case CppNodeKind::Case:
            case CppNodeKind::Default:
            case CppNodeKind::Try:
            case CppNodeKind::Catch:
            case CppNodeKind::Label:
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                WalkStatements(node->body);
                for (const CppNodePtr &handler : node->handlers) WalkStatement(handler.get());
                break;
            case CppNodeKind::ExprStmt:
                CheckSelfAssignment(node);
                CheckResultUnused(node);
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                WalkStatements(node->body);
                break;
            case CppNodeKind::DeclStmt:
                for (const CppNodePtr &kid : node->kids) WalkDeclaration(kid.get());
                break;
            case CppNodeKind::Return:
            case CppNodeKind::CoReturn:
                for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
                break;
            default:
                break;
        }
    }

    /** @brief Walks an expression for the checks that are about calls and lambdas rather than statements. */
    void WalkExpressionStatements(const CppNode *node) {
        if (node == nullptr) return;
        if (node->kind == CppNodeKind::Call) CheckCallArguments(node);
        if (node->kind == CppNodeKind::Lambda) WalkStatements(node->body);
        for (const CppNodePtr &kid : node->kids) WalkExpressionStatements(kid.get());
        for (const CppNodePtr &stmt : node->body) {
            if (node->kind != CppNodeKind::Lambda) WalkStatement(stmt.get());
        }
    }

    const CppNode *ConditionOf(const CppNode *node) const {
        if (node->kind == CppNodeKind::If) return node->kids.size() > 1 ? node->kids[1].get() : nullptr;
        if (node->kind == CppNodeKind::While || node->kind == CppNodeKind::DoWhile) {
            return node->kids.empty() ? nullptr : node->kids.front().get();
        }
        return nullptr;
    }

    void CheckConditionAssignment(const CppNode *node) {
        const CppNode *condition = ConditionOf(node);
        if (condition == nullptr || condition->kind != CppNodeKind::Binary || condition->name != "=") return;
        // `if ((x = f()))` -- the extra parentheses are how a person says
        // they meant it, and every compiler accepts them as such.
        Report("assignment-in-condition",
               "Assignment used as a condition: write `==` to compare, or wrap it in another pair of parentheses "
               "to say the assignment is intended",
               condition->name_pos.line == 0 && condition->name_pos.col == 0 ? condition->start
                                                                             : condition->name_pos,
               1, CppLspSeverity::Warning);
    }

    void CheckEmptyBody(const CppNode *node) {
        if (node->body.size() != 1) return;
        const CppNode *body = node->body.front().get();
        if (body == nullptr || body->kind != CppNodeKind::Empty) return;
        const char *what = node->kind == CppNodeKind::If ? "if" : "while";
        Report("empty-body",
               std::string("This `") + what + "` has an empty body: the `;` ends it, and the block below runs "
                                              "either way",
               body->start, 1, CppLspSeverity::Warning);
    }

    void CheckDuplicateConditions(const CppNode *node) {
        std::vector<std::pair<std::string, const CppNode *>> conditions;
        const CppNode *current = node;
        int guard = 0;
        while (current != nullptr && current->kind == CppNodeKind::If && guard++ < 200) {
            const CppNode *condition = ConditionOf(current);
            const std::string rendered = RenderExpression(condition);
            if (!rendered.empty() && !HasSideEffect(condition)) conditions.emplace_back(rendered, condition);
            current = current->orelse.size() == 1 ? current->orelse.front().get() : nullptr;
        }
        for (size_t i = 1; i < conditions.size(); i++) {
            for (size_t j = 0; j < i; j++) {
                if (conditions[i].first != conditions[j].first) continue;
                Report("duplicate-condition",
                       "This condition is the same as the one on line " +
                           std::to_string(conditions[j].second->start.line + 1) + ", so this branch never runs",
                       conditions[i].second->start, 1, CppLspSeverity::Warning);
                return;
            }
        }
    }

    void CheckSelfAssignment(const CppNode *node) {
        if (node->kids.empty()) return;
        const CppNode *expr = node->kids.front().get();
        if (expr == nullptr || expr->kind != CppNodeKind::Binary || expr->name != "=") return;
        if (expr->kids.size() < 2) return;
        const std::string left = RenderExpression(expr->kids[0].get());
        const std::string right = RenderExpression(expr->kids[1].get());
        if (left.empty() || left != right) return;
        Report("self-assignment", "'" + left + "' is assigned to itself", expr->start,
               static_cast<int>(left.size()), CppLspSeverity::Warning);
    }

    void CheckResultUnused(const CppNode *node) {
        if (node->kids.empty()) return;
        const CppNode *expr = node->kids.front().get();
        if (expr == nullptr || expr->kind != CppNodeKind::Binary) return;
        static const std::set<std::string> kPureOps = {"==", "!=", "<", ">", "<=", ">=", "&&", "||",
                                                       "+",  "-",  "*", "/", "%",  "&",  "|",  "^"};
        if (kPureOps.count(expr->name) == 0) return;
        if (HasSideEffect(expr)) return;
        Report("expression-has-no-effect",
               "This expression is computed and thrown away; did you mean `=` rather than `" + expr->name + "`?",
               expr->start, 1, CppLspSeverity::Warning);
    }

    void CheckUnreachableCode(const std::vector<CppNodePtr> &body) {
        for (size_t i = 0; i + 1 < body.size(); i++) {
            const CppNode *stmt = body[i].get();
            if (stmt == nullptr) continue;
            bool terminates = stmt->kind == CppNodeKind::Return || stmt->kind == CppNodeKind::CoReturn ||
                              stmt->kind == CppNodeKind::Break || stmt->kind == CppNodeKind::Continue ||
                              stmt->kind == CppNodeKind::Goto;
            if (stmt->kind == CppNodeKind::ExprStmt && !stmt->kids.empty() &&
                stmt->kids.front() != nullptr && stmt->kids.front()->kind == CppNodeKind::Throw) {
                terminates = true;
            }
            if (!terminates) continue;
            const CppNode *next = body[i + 1].get();
            if (next == nullptr) continue;
            // A label, a case and a declaration can all be reached from
            // somewhere else, and an empty statement is nothing at all.
            if (next->kind == CppNodeKind::Label || next->kind == CppNodeKind::Case ||
                next->kind == CppNodeKind::Default || next->kind == CppNodeKind::Empty ||
                next->kind == CppNodeKind::Class || next->kind == CppNodeKind::Function ||
                next->kind == CppNodeKind::DeclStmt) {
                continue;
            }
            Report("unreachable-code",
                   "This statement cannot be reached: the one on line " + std::to_string(stmt->start.line + 1) +
                       " always leaves",
                   next->start, 1, CppLspSeverity::Warning);
            return;
        }
    }

    void CheckClass(const CppNode *node) {
        if (!node->is_definition) return;
        bool has_virtual = false;
        const CppNode *destructor = nullptr;
        for (const CppNodePtr &member : node->body) {
            if (member == nullptr || member->kind != CppNodeKind::Function) continue;
            if (member->is_virtual || member->is_pure) has_virtual = true;
            if (member->name.rfind("~", 0) == 0) destructor = member.get();
        }
        if (!has_virtual) return;
        // A class that derives from something inherits its destructor's
        // virtualness, and this server cannot see the base's -- so only a
        // class with no base clause at all is reported.
        if (!node->bases.empty()) return;
        if (destructor != nullptr && (destructor->is_virtual || destructor->str_value == "private")) return;
        if (destructor != nullptr && destructor->str_value == "protected") return;
        Report("non-virtual-destructor",
               "'" + node->name +
                   "' has virtual functions but " +
                   (destructor == nullptr ? "no destructor" : "a public non-virtual destructor") +
                   ": deleting a derived object through a base pointer would be undefined",
               node->name_pos, static_cast<int>(std::max<size_t>(1, node->name.size())), CppLspSeverity::Warning);
    }

    void CheckConstructorInitOrder(const CppNode *node) {
        if (node->kids.size() < 2) return;
        // The class this constructor belongs to has to be visible here:
        // an out-of-line constructor's fields are in the class's scope.
        const int symbol = FindSymbolAt(node->name_pos);
        if (symbol < 0) return;
        const CppSymbol &constructor = index_.symbols[static_cast<size_t>(symbol)];
        if (constructor.kind != CppSymbolKind::Constructor) return;
        const CppScope &class_scope = index_.scopes[static_cast<size_t>(constructor.scope)];
        if (class_scope.kind != CppScope::Kind::Class) return;
        std::unordered_map<std::string, int> field_order;
        int order = 0;
        for (int member : class_scope.symbols) {
            const CppSymbol &field = index_.symbols[static_cast<size_t>(member)];
            if (field.kind == CppSymbolKind::Field && !field.is_static) field_order[field.name] = order++;
        }
        int previous = -1;
        std::string previous_name;
        for (size_t i = 1; i < node->kids.size(); i++) {
            const CppNode *init = node->kids[i].get();
            if (init == nullptr || init->kind != CppNodeKind::MemberInit) continue;
            const auto found = field_order.find(init->name);
            if (found == field_order.end()) continue;  // a base class, or a field from elsewhere
            if (found->second < previous) {
                Report("member-init-order",
                       "'" + init->name + "' is initialized after '" + previous_name +
                           "' here, but declared before it: members are initialized in declaration order",
                       init->name_pos, static_cast<int>(init->name.size()), CppLspSeverity::Warning);
                return;
            }
            previous = found->second;
            previous_name = init->name;
        }
    }

    int FindSymbolAt(const CppPos &pos) const {
        for (size_t i = 0; i < index_.symbols.size(); i++) {
            const CppSymbol &symbol = index_.symbols[i];
            if (symbol.pos.line == pos.line && symbol.pos.col == pos.col) return static_cast<int>(i);
        }
        return -1;
    }

    // --- Calls and members --------------------------------------------

    void CheckCallArguments(const CppNode *node) {
        if (!tree_is_sound_ || node->kids.empty()) return;
        const CppNode *callee = node->kids.front().get();
        if (callee == nullptr || callee->kind != CppNodeKind::Id) return;
        const std::string name = callee->name;
        if (name.empty() || name_counts_[name] != 1) return;  // overloads: say nothing
        const int scope = CppScopeAt(index_, node->start);
        const int symbol_index = CppLookupName(index_, scope, name);
        if (symbol_index < 0) return;
        const CppSymbol &symbol = index_.symbols[static_cast<size_t>(symbol_index)];
        if (symbol.external || symbol.node == nullptr) return;
        if (symbol.kind != CppSymbolKind::Function && symbol.kind != CppSymbolKind::Method) return;
        if (symbol.is_template || symbol.node->is_template) return;
        // `WebSocket socket(sock_fd);` is a function declaration by the
        // grammar and a variable by intent (see cpp_parse.cpp on the most
        // vexing parse). An unnamed parameter whose type is a name this
        // file does not declare as a type is that shape, and counting
        // arguments against it would be counting against a variable.
        for (const CppNodePtr &param : symbol.node->params) {
            if (param == nullptr || !param->name.empty() || param->kids.empty()) continue;
            const CppNode *type = param->kids.front().get();
            if (type == nullptr) continue;
            const std::string base = type->type_base.empty() ? type->type_text : type->type_base;
            if (base.empty() || IsCppFundamentalType(base)) continue;
            if (CppResolveTypeName(index_, symbol.scope, base) < 0) return;
        }
        size_t required = 0;
        size_t accepted = 0;
        // `int f(void)` declares no parameters at all, which matters
        // here because C headers spell it that way.
        if (symbol.node->params.size() == 1 && symbol.node->params.front() != nullptr &&
            symbol.node->params.front()->name.empty() && !symbol.node->params.front()->kids.empty() &&
            CppTypeText(symbol.node->params.front()->kids.front().get()) == "void") {
            if (node->kids.size() == 1) return;
        }
        for (const CppNodePtr &param : symbol.node->params) {
            if (param == nullptr) continue;
            if (param->is_variadic) return;  // `...` accepts anything
            accepted++;
            if (param->kids.size() < 2) required++;
        }
        const size_t given = node->kids.size() - 1;
        if (given >= required && given <= accepted) return;
        Report("wrong-argument-count",
               "'" + name + "' takes " +
                   (required == accepted ? std::to_string(required)
                                         : std::to_string(required) + " to " + std::to_string(accepted)) +
                   (accepted == 1 ? " argument" : " arguments") + ", but " + std::to_string(given) +
                   (given == 1 ? " was" : " were") + " given",
               callee->name_pos, static_cast<int>(name.size()), CppLspSeverity::Warning);
    }

    void CheckMemberAccesses() {
        if (!tree_is_sound_) return;
        for (const CppOccurrence &occurrence : index_.occurrences) {
            if (!occurrence.is_member || occurrence.symbol >= 0 || occurrence.owner_type < 0) continue;
            const CppSymbol &owner = index_.symbols[static_cast<size_t>(occurrence.owner_type)];
            if (owner.external || owner.child_scope < 0) continue;
            const CppScope &class_scope = index_.scopes[static_cast<size_t>(owner.child_scope)];
            // Only when the class is entirely visible: a base class from
            // a header this server never read could have the member.
            if (!owner.is_definition || !class_scope.unresolved_bases.empty()) continue;
            if (owner.is_template) continue;
            std::string suggestion = NearestMember(owner.child_scope, occurrence.name);
            Report("no-such-member",
                   "'" + owner.name + "' has no member '" + occurrence.name + "'" +
                       (suggestion.empty() ? "" : "; did you mean '" + suggestion + "'?"),
                   occurrence.pos, occurrence.length, CppLspSeverity::Warning);
        }
    }

    std::string NearestMember(int class_scope, const std::string &name) const {
        std::string best;
        int best_distance = 3;
        for (int member : index_.scopes[static_cast<size_t>(class_scope)].symbols) {
            const CppSymbol &candidate = index_.symbols[static_cast<size_t>(member)];
            if (candidate.name.empty()) continue;
            const int distance = EditDistance(name, candidate.name, best_distance);
            if (distance < best_distance) {
                best_distance = distance;
                best = candidate.name;
            }
        }
        return best;
    }

    // --- Undefined names ----------------------------------------------

    void CheckUndefinedNames() {
        // Every condition here is a reason this check would otherwise be
        // wrong, and each one really happens:
        //   - a header this server could not read declares the name;
        //   - `import` of a module, whose exports are invisible here;
        //   - `using namespace X;` where X is not in this file;
        //   - the file does not parse cleanly, so the tree is partial;
        //   - the file is a header fragment with no declarations at all.
        if (!tree_is_sound_ || index_.has_unresolved_include || index_.has_unknown_using_directive) return;
        if (index_.imports_module) return;
        std::map<std::string, const CppOccurrence *> unplaceable;
        for (const CppOccurrence &occurrence : index_.occurrences) {
            if (occurrence.symbol >= 0 || occurrence.is_member || occurrence.is_declaration) continue;
            if (occurrence.name.empty()) continue;
            if (IsCppKeyword(occurrence.name) || CppLspIsKnownName(occurrence.name)) continue;
            if (index_.parse.macros.count(occurrence.name) != 0) continue;
            if (unplaceable.count(occurrence.name) == 0) unplaceable[occurrence.name] = &occurrence;
        }
        // Six distinct names this server cannot place is what a file
        // built around something it cannot see looks like from the
        // inside; reporting them one by one would be noise, so the whole
        // check stands down. (The same threshold, for the same reason, as
        // the R server's -- see r_lsp.cpp.)
        if (unplaceable.size() >= 6) return;
        for (const auto &entry : unplaceable) {
            const CppOccurrence &occurrence = *entry.second;
            const std::string suggestion = NearestVisibleName(occurrence);
            Report("undefined-name",
                   "'" + occurrence.name + "' is not declared in this file" +
                       (suggestion.empty() ? "" : "; did you mean '" + suggestion + "'?"),
                   occurrence.pos, occurrence.length, CppLspSeverity::Warning);
        }
    }

    std::string NearestVisibleName(const CppOccurrence &occurrence) const {
        std::string best;
        int best_distance = 3;
        for (const CppSymbol &symbol : index_.symbols) {
            if (symbol.name.empty() || symbol.name == occurrence.name) continue;
            const int distance = EditDistance(occurrence.name, symbol.name, best_distance);
            if (distance < best_distance) {
                best_distance = distance;
                best = symbol.name;
            }
        }
        return best;
    }

    // --- Style hints, calibrated to the document ----------------------

    void CheckNullLiteral() {
        // Only where the file has already decided: a codebase that uses
        // `nullptr` everywhere and `NULL` once wants to know about the
        // one. A file that uses NULL throughout is a C-shaped file, and
        // this stays quiet in it.
        int nullptr_count = 0;
        std::vector<const CppToken *> null_tokens;
        for (const CppToken &token : index_.parse.tokens) {
            if (token.kind == CppTokKind::Keyword && token.text == "nullptr") nullptr_count++;
            if (token.kind == CppTokKind::Ident && token.text == "NULL" && !token.from_macro) {
                null_tokens.push_back(&token);
            }
        }
        if (nullptr_count == 0 || null_tokens.empty()) return;
        if (static_cast<int>(null_tokens.size()) > nullptr_count) return;
        for (const CppToken *token : null_tokens) {
            Report("prefer-nullptr", "This file uses `nullptr` elsewhere; `NULL` is a macro for 0", token->start, 4,
                   CppLspSeverity::Hint);
        }
    }

    void CheckLineLength() {
        if (opts_.max_line_length <= 0) return;
        for (size_t i = 0; i < index_.lines.size(); i++) {
            const std::string &line = index_.lines[i];
            if (static_cast<int>(line.size()) <= opts_.max_line_length) continue;
            CppPos pos;
            pos.line = static_cast<int>(i);
            pos.col = opts_.max_line_length;
            Report("line-too-long",
                   "Line is " + std::to_string(line.size()) + " columns, over the " +
                       std::to_string(opts_.max_line_length) + " this project asked for",
                   pos, static_cast<int>(line.size()) - opts_.max_line_length, CppLspSeverity::Hint);
        }
    }
};

}  // namespace

std::vector<CppLspDiagnostic> CppLspDiagnostics(const std::vector<std::string> &lines, const CppLspOptions &opts) {
    const CppIndex index = BuildCppIndex(lines, opts);
    DiagnosticCollector collector(index, opts);
    return collector.Run();
}
