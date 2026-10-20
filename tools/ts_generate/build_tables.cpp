// The table pipeline: a port of build_tables/{mod,token_conflicts,
// coincident_tokens}.rs.
#include <algorithm>
#include <set>

#include "build_tables.h"
#include "regex.h"

namespace tsgen {

// ---- tables.rs --------------------------------------------------------------------

std::strong_ordering ParseAction::operator<=>(const ParseAction &o) const {
    if (kind != o.kind) return kind <=> o.kind;
    if (kind == Kind::Shift) {
        if (auto c = state <=> o.state; c != 0) return c;
        return is_repetition <=> o.is_repetition;
    }
    if (kind == Kind::Reduce) {
        if (auto c = symbol <=> o.symbol; c != 0) return c;
        if (auto c = child_count <=> o.child_count; c != 0) return c;
        if (auto c = dynamic_precedence <=> o.dynamic_precedence; c != 0) return c;
        return production_id <=> o.production_id;
    }
    return std::strong_ordering::equal;
}

std::vector<size_t> ParseState::ReferencedStates() const {
    std::vector<size_t> result;
    for (const auto &[symbol, entry] : terminal_entries.Entries())
        for (const ParseAction &a : entry.actions)
            if (a.kind == ParseAction::Kind::Shift) result.push_back(a.state);
    for (const auto &[symbol, action] : nonterminal_entries.Entries())
        if (!action.shift_extra) result.push_back(action.state);
    return result;
}

std::strong_ordering LexState::operator<=>(const LexState &o) const {
    if (accept_action.has_value() != o.accept_action.has_value()) return accept_action.has_value() <=> o.accept_action.has_value();
    if (accept_action)
        if (auto c = *accept_action <=> *o.accept_action; c != 0) return c;
    if (eof_action.has_value() != o.eof_action.has_value()) return eof_action.has_value() <=> o.eof_action.has_value();
    if (eof_action)
        if (auto c = *eof_action <=> *o.eof_action; c != 0) return c;
    size_t n = std::min(advance_actions.size(), o.advance_actions.size());
    for (size_t i = 0; i < n; i++) {
        if (auto c = advance_actions[i].first <=> o.advance_actions[i].first; c != 0) return c;
        if (auto c = advance_actions[i].second <=> o.advance_actions[i].second; c != 0) return c;
    }
    return advance_actions.size() <=> o.advance_actions.size();
}

// ---- token_conflicts.rs ---------------------------------------------------------------

bool TokenConflictMap::PreferToken(const LexicalGrammar &grammar, std::pair<int32_t, size_t> left,
                                   std::pair<int32_t, size_t> right) {
    if (left.first != right.first) return left.first > right.first;
    int32_t li = grammar.variables[left.second].implicit_precedence, ri = grammar.variables[right.second].implicit_precedence;
    if (li != ri) return li > ri;
    return left.second < right.second;
}

bool TokenConflictMap::PreferTransition(const LexicalGrammar &grammar, const NfaTransition &t, size_t completed_id,
                                        int32_t completed_precedence, bool has_separator_transitions) {
    if (t.precedence < completed_precedence) return false;
    if (t.precedence == completed_precedence) {
        if (t.is_separator) return false;
        if (has_separator_transitions) {
            bool any = false;
            for (size_t i : grammar.VariableIndicesForNfaStates(t.states))
                if (i == completed_id) any = true;
            if (!any) return false;
        }
    }
    return true;
}

namespace {

std::pair<TokenConflictStatus, TokenConflictStatus> ComputeConflictStatus(NfaCursor &cursor, const LexicalGrammar &grammar,
                                                                          const std::vector<CharacterSet> &following_chars,
                                                                          size_t i, size_t j) {
    std::set<std::vector<uint32_t>> visited;
    std::vector<std::vector<uint32_t>> queue{{grammar.variables[i].start_state, grammar.variables[j].start_state}};
    std::pair<TokenConflictStatus, TokenConflictStatus> result;
    while (!queue.empty()) {
        std::vector<uint32_t> state_set = std::move(queue.back());
        queue.pop_back();
        std::vector<size_t> live = grammar.VariableIndicesForNfaStates(state_set);
        if (live.size() == 1) {
            if (live[0] == i) result.first.matches_different_string = true;
            else result.second.matches_different_string = true;
            continue;
        }
        cursor.Reset(std::move(state_set));
        bool within_separator = false;
        for (const auto &[chars, sep] : cursor.TransitionChars())
            if (sep) within_separator = true;
        std::optional<std::pair<size_t, int32_t>> completion;
        for (const auto &[id, precedence] : cursor.Completions()) {
            if (within_separator) {
                if (id == i) result.first.does_match_separators = true;
                else result.second.does_match_separators = true;
            }
            if (completion) {
                auto [prev_id, prev_precedence] = *completion;
                if (id == prev_id) continue;
                size_t preferred_id;
                if (TokenConflictMap::PreferToken(grammar, {prev_precedence, prev_id}, {precedence, id})) {
                    preferred_id = prev_id;
                } else {
                    preferred_id = id;
                    completion = {id, precedence};
                }
                if (preferred_id == i) result.first.matches_same_string = true;
                else result.second.matches_same_string = true;
            } else {
                completion = {id, precedence};
            }
        }
        for (NfaTransition &transition : cursor.Transitions()) {
            bool can_advance = true;
            if (completion) {
                auto [completed_id, completed_precedence] = *completion;
                std::optional<size_t> advanced_id;
                bool successor_contains_completed_id = false;
                for (size_t variable_id : grammar.VariableIndicesForNfaStates(transition.states)) {
                    if (variable_id == completed_id) {
                        successor_contains_completed_id = true;
                        break;
                    }
                    advanced_id = variable_id;
                }
                if (advanced_id && !successor_contains_completed_id) {
                    if (TokenConflictMap::PreferTransition(grammar, transition, completed_id, completed_precedence, within_separator)) {
                        can_advance = true;
                        if (*advanced_id == i) {
                            result.first.does_match_continuation = true;
                            if (transition.characters.DoesIntersect(following_chars[j])) result.first.does_match_valid_continuation = true;
                        } else {
                            result.second.does_match_continuation = true;
                            if (transition.characters.DoesIntersect(following_chars[i])) result.second.does_match_valid_continuation = true;
                        }
                    } else if (completed_id == i) {
                        result.first.matches_prefix = true;
                    } else {
                        result.second.matches_prefix = true;
                    }
                }
            }
            if (can_advance && visited.insert(transition.states).second) queue.push_back(std::move(transition.states));
        }
    }
    return result;
}

}  // namespace

TokenConflictMap::TokenConflictMap(const LexicalGrammar &grammar, std::vector<TokenSet> following_tokens) {
    NfaCursor cursor(&grammar.nfa);
    std::vector<CharacterSet> starting_chars;
    for (const LexicalVariable &v : grammar.variables) {
        cursor.Reset({v.start_state});
        CharacterSet all;
        for (const auto &[chars, sep] : cursor.TransitionChars()) all = all.Add(*chars);
        starting_chars.push_back(std::move(all));
    }
    std::vector<CharacterSet> following_chars;
    for (const TokenSet &tokens : following_tokens) {
        CharacterSet chars;
        for (const Symbol &t : tokens.Symbols())
            if (t.IsTerminal()) chars = chars.Add(starting_chars[t.index]);
        following_chars.push_back(std::move(chars));
    }
    n_ = grammar.variables.size();
    status_matrix_.assign(n_ * n_, TokenConflictStatus{});
    for (size_t i = 0; i < n_; i++) {
        for (size_t j = 0; j < i; j++) {
            auto status = ComputeConflictStatus(cursor, grammar, following_chars, i, j);
            status_matrix_[n_ * i + j] = status.first;
            status_matrix_[n_ * j + i] = status.second;
        }
    }
}

// ---- coincident_tokens.rs ---------------------------------------------------------------

CoincidentTokenIndex::CoincidentTokenIndex(const ParseTable &table, const LexicalGrammar &lexical)
    : n_(lexical.variables.size()), entries_(n_ * n_) {
    for (size_t i = 0; i < table.states.size(); i++) {
        const auto &entries = table.states[i].terminal_entries.Entries();
        for (const auto &[symbol, e1] : entries) {
            if (!symbol.IsTerminal()) continue;
            for (const auto &[other, e2] : entries) {
                if (!other.IsTerminal()) continue;
                std::vector<size_t> &states = entries_[Index(symbol.index, other.index)];
                if (states.empty() || states.back() != i) states.push_back(i);
            }
        }
    }
}

// ---- mod.rs -------------------------------------------------------------------------------

namespace {

std::vector<TokenSet> GetFollowingTokens(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                         const InlinedProductionMap &inlines, const ParseItemSetBuilder &builder) {
    std::vector<TokenSet> result(lexical.variables.size());
    TokenSet all_tokens;
    for (size_t i = 0; i < result.size(); i++) all_tokens.Insert(Symbol::Terminal(i));
    auto visit = [&](const Production &production) {
        for (size_t i = 1; i < production.steps.size(); i++) {
            const TokenSet &left_tokens = builder.LastSet(production.steps[i - 1].symbol);
            const TokenSet &right_tokens = builder.FirstSet(production.steps[i].symbol);
            const TokenSet *right_reserved = builder.ReservedFirstSet(production.steps[i].symbol);
            for (const Symbol &left : left_tokens.Symbols()) {
                if (!left.IsTerminal()) continue;
                result[left.index].InsertAllTerminals(right_tokens);
                if (right_reserved) result[left.index].InsertAllTerminals(*right_reserved);
            }
        }
    };
    for (const SyntaxVariable &v : syntax.variables)
        for (const Production &p : v.productions) visit(p);
    for (const auto &p : inlines.productions) visit(*p);
    for (const Symbol &extra : syntax.extra_symbols) {
        if (!extra.IsTerminal()) continue;
        for (TokenSet &entry : result) entry.Insert(extra);
        result[extra.index] = all_tokens;
    }
    return result;
}

bool AllCharsAreAlphabetical(const NfaCursor &cursor) {
    for (const auto &[chars, is_sep] : cursor.TransitionChars()) {
        if (is_sep) continue;
        for (const auto &r : chars->ranges) {
            for (uint32_t c = r.first; c < r.second; c++) {
                if (c >= 0xD800 && c <= 0xDFFF) continue;
                if (!(regex::IsAlphabetic(c) || c == '_')) return false;
            }
        }
    }
    return true;
}

TokenSet IdentifyKeywords(const LexicalGrammar &lexical, const ParseTable &table, std::optional<Symbol> word_token,
                          const TokenConflictMap &conflicts, const CoincidentTokenIndex &coincident) {
    if (!word_token) return {};
    NfaCursor cursor(&lexical.nfa);
    TokenSet candidates;
    for (size_t i = 0; i < lexical.variables.size(); i++) {
        cursor.Reset({lexical.variables[i].start_state});
        if (AllCharsAreAlphabetical(cursor) && conflicts.DoesMatchSameString(i, word_token->index) &&
            !conflicts.DoesMatchDifferentString(i, word_token->index))
            candidates.Insert(Symbol::Terminal(i));
    }
    TokenSet keywords;
    for (const Symbol &token : candidates.Symbols()) {
        bool keep = true;
        for (const Symbol &other : candidates.Symbols()) {
            if (other != token && conflicts.DoesMatchSameString(other.index, token.index)) {
                keep = false;
                break;
            }
        }
        if (keep) keywords.Insert(token);
    }
    TokenSet result;
    for (const Symbol &token : keywords.Symbols()) {
        bool keep = true;
        for (size_t other_index = 0; other_index < lexical.variables.size(); other_index++) {
            if (candidates.Contains(Symbol::Terminal(other_index))) continue;
            bool all = true;
            for (size_t state_id : coincident.StatesWith(token, Symbol::Terminal(other_index)))
                if (!table.states[state_id].terminal_entries.Contains(*word_token)) {
                    all = false;
                    break;
                }
            if (all) continue;
            if (!conflicts.HasSameConflictStatus(token.index, word_token->index, other_index)) {
                keep = false;
                break;
            }
        }
        if (keep) result.Insert(token);
    }
    return result;
}

void PopulateErrorState(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                        const CoincidentTokenIndex &coincident, const TokenConflictMap &conflicts, const TokenSet &keywords) {
    ParseState &state = table.states[0];
    size_t n = lexical.variables.size();
    TokenSet conflict_free_tokens;
    for (size_t i = 0; i < n; i++) {
        bool conflicts_with_other = false;
        for (size_t j = 0; j < n; j++) {
            if (j != i && !coincident.Contains(Symbol::Terminal(i), Symbol::Terminal(j)) && conflicts.DoesMatchShorterOrLonger(i, j)) {
                conflicts_with_other = true;
                break;
            }
        }
        if (!conflicts_with_other) conflict_free_tokens.Insert(Symbol::Terminal(i));
    }
    ParseAction recover;
    recover.kind = ParseAction::Kind::Recover;
    ParseTableEntry recover_entry{{recover}, false};
    std::vector<Symbol> conflict_free = conflict_free_tokens.Symbols();
    for (size_t i = 0; i < n; i++) {
        Symbol symbol = Symbol::Terminal(i);
        if (!conflict_free_tokens.Contains(symbol) && !keywords.Contains(symbol) && syntax.word_token != std::optional<Symbol>(symbol)) {
            bool excluded = false;
            for (const Symbol &t : conflict_free) {
                if (!coincident.Contains(symbol, t) && conflicts.DoesConflict(symbol.index, t.index)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) continue;
        }
        state.terminal_entries.OrInsert(symbol, recover_entry);
    }
    for (size_t i = 0; i < syntax.external_tokens.size(); i++)
        if (!syntax.external_tokens[i].corresponding_internal_token) state.terminal_entries.OrInsert(Symbol::External(i), recover_entry);
    state.terminal_entries.Insert(Symbol::End(), recover_entry);
}

void PopulateUsedSymbols(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical) {
    std::vector<bool> terminal_usages(lexical.variables.size()), non_terminal_usages(syntax.variables.size()),
        external_usages(syntax.external_tokens.size());
    for (const ParseState &state : table.states) {
        for (const auto &[symbol, entry] : state.terminal_entries.Entries()) {
            if (symbol.IsTerminal()) terminal_usages[symbol.index] = true;
            else if (symbol.IsExternal()) external_usages[symbol.index] = true;
        }
        for (const auto &[symbol, action] : state.nonterminal_entries.Entries()) non_terminal_usages[symbol.index] = true;
    }
    table.symbols.push_back(Symbol::End());
    for (size_t i = 0; i < terminal_usages.size(); i++) {
        if (!terminal_usages[i]) continue;
        if (syntax.word_token && syntax.word_token->index == i) table.symbols.insert(table.symbols.begin() + 1, Symbol::Terminal(i));
        else table.symbols.push_back(Symbol::Terminal(i));
    }
    for (size_t i = 0; i < external_usages.size(); i++)
        if (external_usages[i]) table.symbols.push_back(Symbol::External(i));
    for (size_t i = 0; i < non_terminal_usages.size(); i++)
        if (non_terminal_usages[i]) table.symbols.push_back(Symbol::NonTerminal(i));
}

void PopulateExternalLexStates(ParseTable &table, const SyntaxGrammar &syntax) {
    std::unordered_map<size_t, size_t> by_internal;
    for (size_t i = 0; i < syntax.external_tokens.size(); i++)
        if (auto s = syntax.external_tokens[i].corresponding_internal_token) by_internal[s->index] = i;
    table.external_lex_states.push_back(TokenSet{});
    for (ParseState &state : table.states) {
        TokenSet external_tokens;
        for (const auto &[token, entry] : state.terminal_entries.Entries()) {
            if (token.IsExternal()) {
                external_tokens.Insert(token);
            } else if (token.IsTerminal()) {
                auto it = by_internal.find(token.index);
                if (it != by_internal.end()) external_tokens.Insert(Symbol::External(it->second));
            }
        }
        size_t id = table.external_lex_states.size();
        for (size_t k = 0; k < table.external_lex_states.size(); k++)
            if (table.external_lex_states[k] == external_tokens) {
                id = k;
                break;
            }
        if (id == table.external_lex_states.size()) table.external_lex_states.push_back(std::move(external_tokens));
        state.external_lex_state_id = id;
    }
}

void MarkFragileTokens(ParseTable &table, const LexicalGrammar &lexical, const TokenConflictMap &conflicts) {
    size_t n = lexical.variables.size();
    std::vector<bool> valid(n);
    for (ParseState &state : table.states) {
        std::fill(valid.begin(), valid.end(), false);
        for (const auto &[token, entry] : state.terminal_entries.Entries())
            if (token.IsTerminal()) valid[token.index] = true;
        for (auto &[token, entry] : state.terminal_entries.Entries()) {
            if (!token.IsTerminal()) continue;
            for (size_t i = 0; i < n; i++) {
                if (valid[i] && conflicts.DoesOverlap(i, token.index)) {
                    entry.reusable = false;
                    break;
                }
            }
        }
    }
}

}  // namespace

Tables BuildTables(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const AliasMap &simple_aliases,
                   const std::vector<VariableInfo> &variable_info, const InlinedProductionMap &inlines) {
    ParseItemSetBuilder builder(syntax, lexical, inlines);
    std::vector<TokenSet> following_tokens = GetFollowingTokens(syntax, lexical, inlines, builder);
    ParseTable table = BuildParseTable(syntax, lexical, builder, variable_info);
    TokenConflictMap conflicts(lexical, std::move(following_tokens));
    CoincidentTokenIndex coincident(table, lexical);
    TokenSet keywords = IdentifyKeywords(lexical, table, syntax.word_token, conflicts, coincident);
    PopulateErrorState(table, syntax, lexical, coincident, conflicts, keywords);
    PopulateUsedSymbols(table, syntax, lexical);
    MinimizeParseTable(table, syntax, lexical, simple_aliases, conflicts, keywords);
    LexTables lex = BuildLexTable(table, syntax, lexical, keywords, coincident, conflicts);
    PopulateExternalLexStates(table, syntax);
    MarkFragileTokens(table, lexical, conflicts);
    Tables out;
    out.parse_table = std::move(table);
    out.main_lex_table = std::move(lex.main_lex_table);
    out.keyword_lex_table = std::move(lex.keyword_lex_table);
    out.large_character_sets = std::move(lex.large_character_sets);
    return out;
}

}  // namespace tsgen
