// Internals shared by the table builders: build_tables/{item,
// item_set_builder,token_conflicts,coincident_tokens}.rs.
#pragma once

#include "generate.h"

namespace tsgen {

// ---- item.rs ------------------------------------------------------------------

const Production *StartProduction();

struct ParseItem {
    uint32_t variable_index = 0;
    uint32_t step_index = 0;
    const Production *production = nullptr;
    bool has_preceding_inherited_fields = false;

    static ParseItem Start() { return {UINT32_MAX, 0, StartProduction(), false}; }
    const ProductionStep *Step() const {
        return step_index < production->steps.size() ? &production->steps[step_index] : nullptr;
    }
    std::optional<Symbol> NextSymbol() const {
        if (const ProductionStep *s = Step()) return s->symbol;
        return std::nullopt;
    }
    const ProductionStep *PrevStep() const { return step_index > 0 ? &production->steps[step_index - 1] : nullptr; }
    std::optional<Associativity> Assoc() const {
        const ProductionStep *s = PrevStep();
        return s ? s->associativity : std::nullopt;
    }
    const Precedence &Prec() const;
    bool IsDone() const { return step_index == production->steps.size(); }
    bool IsAugmented() const { return variable_index == UINT32_MAX; }
    ParseItem Successor() const { return {variable_index, step_index + 1, production, has_preceding_inherited_fields}; }
    ParseItem SubstituteProduction(const Production *p) const { return {variable_index, step_index, p, has_preceding_inherited_fields}; }

    bool operator==(const ParseItem &o) const;
    std::strong_ordering Cmp(const ParseItem &o) const;  // item.rs's Ord (not consistent with ==)
    size_t Hash() const;
};

struct ParseItemSetEntry {
    ParseItem item;
    TokenSet lookaheads;
    size_t following_reserved_word_set = 0;
    bool operator==(const ParseItemSetEntry &o) const {
        return item == o.item && lookaheads == o.lookaheads && following_reserved_word_set == o.following_reserved_word_set;
    }
};

struct ParseItemSet {
    std::vector<ParseItemSetEntry> entries;
    ParseItemSetEntry &Insert(const ParseItem &item);
    bool operator==(const ParseItemSet &o) const { return entries == o.entries; }
    size_t Hash() const;
};

struct ParseItemSetHash {
    size_t operator()(const ParseItemSet &s) const { return s.Hash(); }
};

struct ParseItemSetCore {
    std::vector<ParseItem> entries;
    bool operator==(const ParseItemSetCore &o) const { return entries == o.entries; }
};

struct ParseItemSetCoreHash {
    size_t operator()(const ParseItemSetCore &c) const;
};

// ---- item_set_builder.rs -------------------------------------------------------

struct FollowSetInfo {
    TokenSet lookaheads;
    size_t reserved_lookaheads = 0;
    bool propagates_lookaheads = false;
    bool operator==(const FollowSetInfo &) const = default;
};

struct TransitiveClosureAddition {
    ParseItem item;
    FollowSetInfo info;
    bool operator==(const TransitiveClosureAddition &o) const { return item == o.item && info == o.info; }
};

class ParseItemSetBuilder {
public:
    ParseItemSetBuilder(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const InlinedProductionMap &inlines);
    ParseItemSet TransitiveClosure(const ParseItemSet &item_set) const;
    const TokenSet &FirstSet(const Symbol &s) const { return first_sets_.at(s); }
    const TokenSet *ReservedFirstSet(const Symbol &s) const;
    const TokenSet &LastSet(const Symbol &s) const { return last_sets_.at(s); }

private:
    void AddItem(ParseItemSet &set, const ParseItemSetEntry &entry) const;

    const SyntaxGrammar &syntax_;
    const InlinedProductionMap &inlines_;
    std::unordered_map<Symbol, TokenSet, SymbolHash> first_sets_;
    std::unordered_map<Symbol, size_t, SymbolHash> reserved_first_sets_;
    std::unordered_map<Symbol, TokenSet, SymbolHash> last_sets_;
    std::vector<std::vector<TransitiveClosureAddition>> transitive_closure_additions_;
};

// ---- build_parse_table.rs -------------------------------------------------------

struct ParseStateInfo {
    std::vector<Symbol> preceding_symbols;
    ParseItemSet item_set;
};

ParseTable BuildParseTable(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const ParseItemSetBuilder &builder,
                           const std::vector<VariableInfo> &variable_info);

// ---- token_conflicts.rs -----------------------------------------------------------

struct TokenConflictStatus {
    bool matches_prefix = false;
    bool does_match_continuation = false;
    bool does_match_valid_continuation = false;
    bool does_match_separators = false;
    bool matches_same_string = false;
    bool matches_different_string = false;
    bool operator==(const TokenConflictStatus &) const = default;
};

class TokenConflictMap {
public:
    TokenConflictMap(const LexicalGrammar &grammar, std::vector<TokenSet> following_tokens);
    bool HasSameConflictStatus(size_t a, size_t b, size_t other) const {
        return At(a, other) == At(b, other);
    }
    bool DoesMatchDifferentString(size_t i, size_t j) const { return At(i, j).matches_different_string; }
    bool DoesMatchSameString(size_t i, size_t j) const { return At(i, j).matches_same_string; }
    bool DoesConflict(size_t i, size_t j) const {
        const auto &e = At(i, j);
        return e.does_match_valid_continuation || e.does_match_separators || e.matches_same_string;
    }
    bool DoesMatchPrefix(size_t i, size_t j) const { return At(i, j).matches_prefix; }
    bool DoesMatchShorterOrLonger(size_t i, size_t j) const {
        const auto &e = At(i, j), &r = At(j, i);
        return (e.does_match_valid_continuation || e.does_match_separators) && !r.does_match_separators;
    }
    bool DoesOverlap(size_t i, size_t j) const {
        const auto &s = At(i, j);
        return s.does_match_separators || s.matches_prefix || s.matches_same_string || s.does_match_continuation;
    }
    static bool PreferToken(const LexicalGrammar &grammar, std::pair<int32_t, size_t> left, std::pair<int32_t, size_t> right);
    static bool PreferTransition(const LexicalGrammar &grammar, const NfaTransition &t, size_t completed_id,
                                 int32_t completed_precedence, bool has_separator_transitions);

private:
    const TokenConflictStatus &At(size_t i, size_t j) const { return status_matrix_[n_ * i + j]; }
    size_t n_;
    std::vector<TokenConflictStatus> status_matrix_;
};

// ---- coincident_tokens.rs -------------------------------------------------------

class CoincidentTokenIndex {
public:
    CoincidentTokenIndex(const ParseTable &table, const LexicalGrammar &lexical);
    const std::vector<size_t> &StatesWith(const Symbol &a, const Symbol &b) const { return entries_[Index(a.index, b.index)]; }
    bool Contains(const Symbol &a, const Symbol &b) const { return !entries_[Index(a.index, b.index)].empty(); }

private:
    size_t Index(size_t a, size_t b) const { return a < b ? a * n_ + b : b * n_ + a; }
    size_t n_;
    std::vector<std::vector<size_t>> entries_;
};

// ---- minimize_parse_table.rs, build_lex_table.rs --------------------------------

void MinimizeParseTable(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                        const AliasMap &simple_aliases, const TokenConflictMap &conflicts, const TokenSet &keywords);

struct LexTables {
    LexTable main_lex_table;
    LexTable keyword_lex_table;
    std::vector<std::pair<std::optional<Symbol>, CharacterSet>> large_character_sets;
};

LexTables BuildLexTable(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                        const TokenSet &keywords, const CoincidentTokenIndex &coincident, const TokenConflictMap &conflicts);

}  // namespace tsgen
