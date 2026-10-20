// Parse-table minimization and the lex tables: a port of
// build_tables/{minimize_parse_table,build_lex_table}.rs.
#include <algorithm>
#include <deque>
#include <map>
#include <set>

#include "build_tables.h"
#include "rust_sort.h"

namespace tsgen {

// ---- minimize_parse_table.rs ------------------------------------------------------

namespace {

class Minimizer {
public:
    Minimizer(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const TokenConflictMap &conflicts,
              const TokenSet &keywords, const AliasMap &simple_aliases)
        : table_(table), syntax_(syntax), conflicts_(conflicts), keywords_(keywords), simple_aliases_(simple_aliases) {
        (void)lexical;
    }

    void Run() {
        MergeCompatibleStates();
        RemoveUnitReductions();
        RemoveUnusedStates();
        ReorderStatesByDescendingSize();
    }

private:
    ParseTable &table_;
    const SyntaxGrammar &syntax_;
    const TokenConflictMap &conflicts_;
    const TokenSet &keywords_;
    const AliasMap &simple_aliases_;

    void RemoveUnitReductions() {
        std::set<Symbol> aliased_symbols;
        for (const SyntaxVariable &v : syntax_.variables)
            for (const Production &p : v.productions)
                for (const ProductionStep &s : p.steps)
                    if (s.alias) aliased_symbols.insert(s.symbol);
        std::unordered_map<size_t, Symbol> unit_reduction_symbols_by_state;
        for (size_t i = 0; i < table_.states.size(); i++) {
            const ParseState &state = table_.states[i];
            bool only_unit_reductions = true;
            std::optional<Symbol> unit_reduction_symbol;
            for (const auto &[token, entry] : state.terminal_entries.Entries()) {
                for (const ParseAction &action : entry.actions) {
                    if (action.kind == ParseAction::Kind::ShiftExtra) continue;
                    if (action.kind == ParseAction::Kind::Reduce && action.child_count == 1 && action.production_id == 0 &&
                        !simple_aliases_.count(action.symbol) && !Contains(syntax_.supertype_symbols, action.symbol) &&
                        !Contains(syntax_.extra_symbols, action.symbol) && !aliased_symbols.count(action.symbol) &&
                        syntax_.variables[action.symbol.index].kind != VariableType::Named &&
                        (!unit_reduction_symbol || *unit_reduction_symbol == action.symbol)) {
                        unit_reduction_symbol = action.symbol;
                        continue;
                    }
                    only_unit_reductions = false;
                    break;
                }
                if (!only_unit_reductions) break;
            }
            if (unit_reduction_symbol && only_unit_reductions) unit_reduction_symbols_by_state[i] = *unit_reduction_symbol;
        }
        for (ParseState &state : table_.states) {
            bool done = false;
            while (!done) {
                done = true;
                state.UpdateReferencedStates([&](size_t other_state_id, const ParseState &s) {
                    auto it = unit_reduction_symbols_by_state.find(other_state_id);
                    if (it == unit_reduction_symbols_by_state.end()) return other_state_id;
                    done = false;
                    const GotoAction *g = s.nonterminal_entries.Get(it->second);
                    if (g && !g->shift_extra) return g->state;
                    return other_state_id;
                });
            }
        }
    }

    void MergeCompatibleStates() {
        size_t core_count = 0;
        for (const ParseState &s : table_.states) core_count = std::max(core_count, s.core_id);
        core_count++;
        std::vector<size_t> group_ids_by_state_id;
        std::vector<std::vector<size_t>> state_ids_by_group_id(core_count);
        for (size_t i = 0; i < table_.states.size(); i++) {
            state_ids_by_group_id[table_.states[i].core_id].push_back(i);
            group_ids_by_state_id.push_back(table_.states[i].core_id);
        }
        SplitStateIdGroups(table_.states, state_ids_by_group_id, group_ids_by_state_id, 0,
                           [&](const ParseState &l, const ParseState &r, const std::vector<size_t> &g) { return StatesConflict(l, r, g); });
        while (SplitStateIdGroups(
            table_.states, state_ids_by_group_id, group_ids_by_state_id, 0,
            [&](const ParseState &l, const ParseState &r, const std::vector<size_t> &g) { return StateSuccessorsDiffer(l, r, g); })) {
        }
        auto position = [&](size_t id) {
            for (size_t g = 0; g < state_ids_by_group_id.size(); g++)
                if (std::find(state_ids_by_group_id[g].begin(), state_ids_by_group_id[g].end(), id) != state_ids_by_group_id[g].end())
                    return g;
            Fail("internal: state in no group");
        };
        size_t error_group_index = position(0);
        size_t start_group_index = position(1);
        std::swap(state_ids_by_group_id[error_group_index], state_ids_by_group_id[0]);
        std::swap(state_ids_by_group_id[start_group_index], state_ids_by_group_id[1]);

        std::vector<ParseState> new_states;
        for (const std::vector<size_t> &state_ids : state_ids_by_group_id) {
            ParseState parse_state = std::move(table_.states[state_ids[0]]);
            table_.states[state_ids[0]] = ParseState{};
            for (size_t k = 1; k < state_ids.size(); k++) {
                ParseState other = std::move(table_.states[state_ids[k]]);
                table_.states[state_ids[k]] = ParseState{};
                parse_state.terminal_entries.Extend(other.terminal_entries);
                parse_state.nonterminal_entries.Extend(other.nonterminal_entries);
                parse_state.reserved_words.InsertAll(other.reserved_words);
                for (const auto &[symbol, entry] : parse_state.terminal_entries.Entries()) parse_state.reserved_words.Remove(symbol);
            }
            parse_state.UpdateReferencedStates([&](size_t id, const ParseState &) { return group_ids_by_state_id[id]; });
            new_states.push_back(std::move(parse_state));
        }
        table_.states = std::move(new_states);
    }

    bool StatesConflict(const ParseState &left, const ParseState &right, const std::vector<size_t> &groups) const {
        for (const auto &[token, left_entry] : left.terminal_entries.Entries()) {
            if (const ParseTableEntry *right_entry = right.terminal_entries.Get(token)) {
                if (EntriesConflict(token, left_entry, *right_entry, groups)) return true;
            } else if (TokenConflicts(right, token)) {
                return true;
            }
        }
        for (const auto &[token, entry] : right.terminal_entries.Entries())
            if (!left.terminal_entries.Contains(token) && TokenConflicts(left, token)) return true;
        return false;
    }

    bool StateSuccessorsDiffer(const ParseState &s1, const ParseState &s2, const std::vector<size_t> &groups) const {
        for (const auto &[token, entry1] : s1.terminal_entries.Entries()) {
            const ParseAction &a1 = entry1.actions.back();
            if (a1.kind != ParseAction::Kind::Shift) continue;
            if (const ParseTableEntry *entry2 = s2.terminal_entries.Get(token)) {
                const ParseAction &a2 = entry2->actions.back();
                if (a2.kind == ParseAction::Kind::Shift && groups[a1.state] != groups[a2.state]) return true;
            }
        }
        for (const auto &[symbol, g1] : s1.nonterminal_entries.Entries()) {
            if (const GotoAction *g2 = s2.nonterminal_entries.Get(symbol)) {
                if (g1.shift_extra && g2->shift_extra) continue;
                if (!g1.shift_extra && !g2->shift_extra) {
                    if (groups[g1.state] != groups[g2->state]) return true;
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    bool EntriesConflict(const Symbol &, const ParseTableEntry &e1, const ParseTableEntry &e2, const std::vector<size_t> &groups) const {
        if (e1.actions.size() != e2.actions.size()) return true;
        for (size_t i = 0; i < e1.actions.size(); i++) {
            const ParseAction &a1 = e1.actions[i], &a2 = e2.actions[i];
            if (a1.kind == ParseAction::Kind::Shift && a2.kind == ParseAction::Kind::Shift) {
                if (groups[a1.state] == groups[a2.state] && a1.is_repetition == a2.is_repetition) continue;
                return true;
            }
            if (!(a1 == a2)) return true;
        }
        return false;
    }

    bool TokenConflicts(const ParseState &right_state, Symbol new_token) const {
        if (new_token == Symbol::EndOfNonTerminalExtra()) return true;
        if (new_token.IsExternal()) return true;
        if (right_state.reserved_words.Contains(new_token)) return false;
        for (const ExternalToken &e : syntax_.external_tokens)
            if (e.corresponding_internal_token == std::optional<Symbol>(new_token)) return true;
        for (const auto &[token, entry] : right_state.terminal_entries.Entries()) {
            if (!token.IsTerminal()) continue;
            if (syntax_.word_token == std::optional<Symbol>(token) && keywords_.Contains(new_token)) continue;
            if (syntax_.word_token == std::optional<Symbol>(new_token) && keywords_.Contains(token)) continue;
            if (conflicts_.DoesConflict(new_token.index, token.index) || conflicts_.DoesMatchSameString(new_token.index, token.index))
                return true;
        }
        return false;
    }

    void RemoveUnusedStates() {
        size_t n = table_.states.size();
        std::vector<bool> used(n, false);
        used[0] = used[1] = true;
        for (const ParseState &s : table_.states)
            for (size_t r : s.ReferencedStates()) used[r] = true;
        std::vector<size_t> replacement(n);
        size_t removed = 0;
        for (size_t id = 0; id < n; id++) {
            replacement[id] = id - removed;
            if (!used[id]) removed++;
        }
        std::vector<ParseState> kept;
        for (size_t id = 0; id < n; id++) {
            if (!used[id]) continue;
            table_.states[id].UpdateReferencedStates([&](size_t other, const ParseState &) { return replacement[other]; });
            kept.push_back(std::move(table_.states[id]));
        }
        table_.states = std::move(kept);
    }

    void ReorderStatesByDescendingSize() {
        size_t n = table_.states.size();
        std::vector<size_t> old_ids_by_new_id(n);
        for (size_t i = 0; i < n; i++) old_ids_by_new_id[i] = i;
        auto key = [&](size_t i) -> int64_t {
            if (i <= 1) return static_cast<int64_t>(i) - 1000000;
            const ParseState &s = table_.states[i];
            return -static_cast<int64_t>(s.terminal_entries.size() + s.nonterminal_entries.size());
        };
        // sort_unstable_by_key on usize: states tying on size end up in the
        // order Rust's ipnsort leaves them.
        RustSortUnstable(old_ids_by_new_id, RustSortType{8, true}, [&](size_t a, size_t b) { return key(a) < key(b); });
        std::vector<size_t> new_ids_by_old_id(n);
        for (size_t id = 0; id < n; id++) new_ids_by_old_id[old_ids_by_new_id[id]] = id;
        std::vector<ParseState> states;
        states.reserve(n);
        for (size_t old_id : old_ids_by_new_id) {
            ParseState state = std::move(table_.states[old_id]);
            state.UpdateReferencedStates([&](size_t id, const ParseState &) { return new_ids_by_old_id[id]; });
            states.push_back(std::move(state));
        }
        table_.states = std::move(states);
    }
};

}  // namespace

void MinimizeParseTable(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                        const AliasMap &simple_aliases, const TokenConflictMap &conflicts, const TokenSet &keywords) {
    Minimizer(table, syntax, lexical, conflicts, keywords, simple_aliases).Run();
}

// ---- build_lex_table.rs -------------------------------------------------------------

namespace {

constexpr size_t kLargeCharacterRangeCount = 8;

struct NfaStateSetKey {
    std::vector<uint32_t> states;
    bool eof_valid;
    bool operator==(const NfaStateSetKey &) const = default;
};

struct NfaStateSetKeyHash {
    size_t operator()(const NfaStateSetKey &k) const {
        size_t h = k.eof_valid;
        for (uint32_t s : k.states) h = h * 1000003 + s;
        return h;
    }
};

class LexTableBuilder {
public:
    explicit LexTableBuilder(const LexicalGrammar &lexical) : lexical_(lexical), cursor_(&lexical.nfa) {}

    void Reset() {
        table = LexTable{};
        queue_.clear();
        ids_.clear();
    }

    size_t AddStateForTokens(const TokenSet &tokens) {
        bool eof_valid = false;
        std::vector<uint32_t> nfa_states;
        for (const Symbol &t : tokens.Symbols()) {
            if (t.IsTerminal()) nfa_states.push_back(lexical_.variables[t.index].start_state);
            else eof_valid = true;
        }
        size_t state_id = AddState(std::move(nfa_states), eof_valid);
        while (!queue_.empty()) {
            QueueEntry e = std::move(queue_.front());
            queue_.pop_front();
            PopulateState(e.state_id, std::move(e.nfa_states), e.eof_valid);
        }
        return state_id;
    }

    LexTable table;

private:
    struct QueueEntry {
        size_t state_id;
        std::vector<uint32_t> nfa_states;
        bool eof_valid;
    };

    const LexicalGrammar &lexical_;
    NfaCursor cursor_;
    std::deque<QueueEntry> queue_;
    std::unordered_map<NfaStateSetKey, size_t, NfaStateSetKeyHash> ids_;

    size_t AddState(std::vector<uint32_t> nfa_states, bool eof_valid) {
        cursor_.Reset(std::move(nfa_states));
        NfaStateSetKey key{cursor_.state_ids, eof_valid};
        auto it = ids_.find(key);
        if (it != ids_.end()) return it->second;
        size_t state_id = table.states.size();
        table.states.push_back(LexState{});
        queue_.push_back({state_id, key.states, eof_valid});
        ids_.emplace(std::move(key), state_id);
        return state_id;
    }

    void PopulateState(size_t state_id, std::vector<uint32_t> nfa_states, bool eof_valid) {
        cursor_.ForceReset(std::move(nfa_states));
        std::optional<std::pair<size_t, int32_t>> completion;
        for (const auto &[id, prec] : cursor_.Completions()) {
            if (completion && TokenConflictMap::PreferToken(lexical_, {completion->second, completion->first}, {prec, id})) continue;
            completion = {id, prec};
        }
        std::vector<NfaTransition> transitions = cursor_.Transitions();
        bool has_sep = false;
        for (const auto &[chars, sep] : cursor_.TransitionChars())
            if (sep) has_sep = true;
        if (eof_valid) {
            size_t next_state_id = AddState({}, false);
            table.states[state_id].eof_action = AdvanceAction{next_state_id, true};
        }
        for (NfaTransition &transition : transitions) {
            if (completion && !TokenConflictMap::PreferTransition(lexical_, transition, completion->first, completion->second, has_sep))
                continue;
            size_t next_state_id = AddState(transition.states, eof_valid && transition.is_separator);
            table.states[state_id].advance_actions.push_back(
                {std::move(transition.characters), AdvanceAction{next_state_id, !transition.is_separator}});
        }
        // The cursor here is the last AddState's (each resets it), as in
        // build_lex_table.rs.
        if (completion) table.states[state_id].accept_action = Symbol::Terminal(completion->first);
        else if (cursor_.state_ids.empty()) table.states[state_id].accept_action = Symbol::End();
    }
};

bool MergeTokenSet(TokenSet &tokens, const TokenSet &other, const LexicalGrammar &lexical, const TokenConflictMap &conflicts,
                   const CoincidentTokenIndex &coincident) {
    for (size_t i = 0; i < lexical.variables.size(); i++) {
        Symbol symbol = Symbol::Terminal(i);
        bool in_tokens = tokens.ContainsTerminal(i), in_other = other.ContainsTerminal(i);
        const TokenSet *set_without_terminal;
        if (in_tokens && !in_other) set_without_terminal = &other;
        else if (!in_tokens && in_other) set_without_terminal = &tokens;
        else continue;
        for (const Symbol &existing : set_without_terminal->Terminals()) {
            if (conflicts.DoesConflict(i, existing.index) || conflicts.DoesMatchPrefix(i, existing.index)) return false;
            if (!coincident.Contains(symbol, existing) &&
                (conflicts.DoesOverlap(existing.index, i) || conflicts.DoesOverlap(i, existing.index)))
                return false;
        }
    }
    tokens.InsertAll(other);
    return true;
}

struct Signature {
    bool is_first;
    std::optional<Symbol> accept_action;
    bool has_eof;
    std::vector<std::pair<CharacterSet, bool>> actions;
    bool operator==(const Signature &) const = default;
};

struct SignatureHash {
    size_t operator()(const Signature &s) const {
        size_t h = s.is_first * 3 + s.has_eof;
        if (s.accept_action) h = h * 31 + SymbolHash()(*s.accept_action) + 1;
        for (const auto &[c, m] : s.actions) h = h * 1000003 + c.Hash() * 2 + m;
        return h;
    }
};

void MinimizeLexTable(LexTable &table, ParseTable &parse_table) {
    std::unordered_map<Signature, std::vector<size_t>, SignatureHash> by_signature;
    for (size_t i = 0; i < table.states.size(); i++) {
        const LexState &state = table.states[i];
        Signature sig{i == 0, state.accept_action, state.eof_action.has_value(), {}};
        for (const auto &[chars, action] : state.advance_actions) sig.actions.push_back({chars, action.in_main_token});
        by_signature[std::move(sig)].push_back(i);
    }
    std::vector<std::vector<size_t>> groups;
    for (auto &[sig, ids] : by_signature) groups.push_back(std::move(ids));
    std::sort(groups.begin(), groups.end());
    size_t error_group = 0;
    for (size_t g = 0; g < groups.size(); g++)
        if (std::find(groups[g].begin(), groups[g].end(), 0) != groups[g].end()) {
            error_group = g;
            break;
        }
    std::swap(groups[error_group], groups[0]);
    std::vector<size_t> group_ids_by_state_id(table.states.size());
    for (size_t g = 0; g < groups.size(); g++)
        for (size_t id : groups[g]) group_ids_by_state_id[id] = g;
    while (SplitStateIdGroups(table.states, groups, group_ids_by_state_id, 1,
                              [](const LexState &l, const LexState &r, const std::vector<size_t> &gids) {
                                  size_t n = std::min(l.advance_actions.size(), r.advance_actions.size());
                                  for (size_t k = 0; k < n; k++)
                                      if (gids[l.advance_actions[k].second.state] != gids[r.advance_actions[k].second.state]) return true;
                                  return false;
                              })) {
    }
    std::vector<LexState> new_states;
    for (const std::vector<size_t> &ids : groups) {
        LexState s = std::move(table.states[ids[0]]);
        table.states[ids[0]] = LexState{};
        for (auto &[chars, action] : s.advance_actions) action.state = group_ids_by_state_id[action.state];
        if (s.eof_action) s.eof_action->state = group_ids_by_state_id[s.eof_action->state];
        new_states.push_back(std::move(s));
    }
    for (ParseState &state : parse_table.states) state.lex_state_id = group_ids_by_state_id[state.lex_state_id];
    table.states = std::move(new_states);
}

void SortStates(LexTable &table, ParseTable &parse_table) {
    size_t n = table.states.size();
    std::vector<size_t> old_ids_by_new_id(n);
    for (size_t i = 0; i < n; i++) old_ids_by_new_id[i] = i;
    std::stable_sort(old_ids_by_new_id.begin() + 1, old_ids_by_new_id.end(),
                     [&](size_t a, size_t b) { return (table.states[a] <=> table.states[b]) < 0; });
    std::vector<size_t> new_ids_by_old_id(n);
    for (size_t id = 0; id < n; id++) new_ids_by_old_id[old_ids_by_new_id[id]] = id;
    std::vector<LexState> states;
    for (size_t old_id : old_ids_by_new_id) {
        LexState s = std::move(table.states[old_id]);
        for (auto &[chars, action] : s.advance_actions) action.state = new_ids_by_old_id[action.state];
        if (s.eof_action) s.eof_action->state = new_ids_by_old_id[s.eof_action->state];
        states.push_back(std::move(s));
    }
    for (ParseState &state : parse_table.states) state.lex_state_id = new_ids_by_old_id[state.lex_state_id];
    table.states = std::move(states);
}

}  // namespace

LexTables BuildLexTable(ParseTable &table, const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                        const TokenSet &keywords, const CoincidentTokenIndex &coincident, const TokenConflictMap &conflicts) {
    LexTables out;
    if (syntax.word_token) {
        LexTableBuilder builder(lexical);
        builder.AddStateForTokens(keywords);
        out.keyword_lex_table = std::move(builder.table);
    }
    std::vector<std::pair<TokenSet, std::vector<size_t>>> parse_state_ids_by_token_set;
    for (size_t i = 0; i < table.states.size(); i++) {
        const ParseState &state = table.states[i];
        TokenSet tokens;
        auto add = [&](const Symbol &token) {
            if (token.IsTerminal()) {
                if (keywords.Contains(token)) {
                    if (syntax.word_token) tokens.Insert(*syntax.word_token);
                } else {
                    tokens.Insert(token);
                }
            } else if (token.IsEof()) {
                tokens.Insert(token);
            }
        };
        for (const auto &[token, entry] : state.terminal_entries.Entries()) add(token);
        for (const Symbol &token : state.reserved_words.Symbols()) add(token);
        bool did_merge = false;
        for (auto &entry : parse_state_ids_by_token_set) {
            if (MergeTokenSet(entry.first, tokens, lexical, conflicts, coincident)) {
                did_merge = true;
                entry.second.push_back(i);
                break;
            }
        }
        if (!did_merge) parse_state_ids_by_token_set.push_back({std::move(tokens), {i}});
    }
    LexTableBuilder builder(lexical);
    for (const auto &[tokens, ids] : parse_state_ids_by_token_set) {
        size_t lex_state_id = builder.AddStateForTokens(tokens);
        for (size_t id : ids) table.states[id].lex_state_id = lex_state_id;
    }
    out.main_lex_table = std::move(builder.table);
    builder.table = LexTable{};
    MinimizeLexTable(out.main_lex_table, table);
    SortStates(out.main_lex_table, table);

    for (size_t variable_ix = 0; variable_ix < lexical.variables.size(); variable_ix++) {
        Symbol symbol = Symbol::Terminal(variable_ix);
        builder.Reset();
        TokenSet only;
        only.Insert(symbol);
        builder.AddStateForTokens(only);
        for (const LexState &state : builder.table.states) {
            CharacterSet characters;
            for (const auto &[chars, action] : state.advance_actions) {
                if (action.in_main_token) {
                    characters = characters.Add(chars);
                    continue;
                }
                if (chars.RangeCount() > kLargeCharacterRangeCount) {
                    bool exists = false;
                    for (const auto &[s, set] : out.large_character_sets)
                        if (set == chars) exists = true;
                    if (!exists) out.large_character_sets.push_back({std::nullopt, chars});
                }
            }
            if (characters.RangeCount() > kLargeCharacterRangeCount) {
                bool exists = false;
                for (const auto &[s, set] : out.large_character_sets)
                    if (set == characters) exists = true;
                if (!exists) out.large_character_sets.push_back({symbol, characters});
            }
        }
    }
    return out;
}

}  // namespace tsgen
