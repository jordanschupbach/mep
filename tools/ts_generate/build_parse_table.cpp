// LR(1) item sets and the parse table: a port of build_tables/{item,
// item_set_builder,build_parse_table}.rs.
#include <algorithm>
#include <deque>
#include <set>

#include "build_tables.h"

namespace tsgen {

// ---- item.rs ------------------------------------------------------------------

const Production *StartProduction() {
    static const Production p = [] {
        Production production;
        ProductionStep step;
        step.symbol = Symbol::NonTerminal(0);
        step.reserved_word_set_id = kNoReservedWords;
        production.steps.push_back(step);
        return production;
    }();
    return &p;
}

const Precedence &ParseItem::Prec() const {
    static const Precedence none;
    const ProductionStep *s = PrevStep();
    return s ? s->precedence : none;
}

template <class T>
static std::strong_ordering CmpOpt(const std::optional<T> &a, const std::optional<T> &b) {
    if (a.has_value() != b.has_value()) return a.has_value() <=> b.has_value();
    if (!a) return std::strong_ordering::equal;
    return *a <=> *b;
}

bool ParseItem::operator==(const ParseItem &o) const {
    if (variable_index != o.variable_index || step_index != o.step_index ||
        production->dynamic_precedence != o.production->dynamic_precedence ||
        production->steps.size() != o.production->steps.size() || !(Prec() == o.Prec()) || Assoc() != o.Assoc() ||
        has_preceding_inherited_fields != o.has_preceding_inherited_fields)
        return false;
    for (size_t i = 0; i < production->steps.size(); i++) {
        const ProductionStep &a = production->steps[i], &b = o.production->steps[i];
        if (i < step_index) {
            if (a.alias != b.alias || a.field_name != b.field_name) return false;
            if (has_preceding_inherited_fields && a.symbol != b.symbol) return false;
        } else if (!(a == b)) {
            return false;
        }
    }
    return true;
}

std::strong_ordering ParseItem::Cmp(const ParseItem &o) const {
    if (auto c = step_index <=> o.step_index; c != 0) return c;
    if (auto c = variable_index <=> o.variable_index; c != 0) return c;
    if (auto c = production->dynamic_precedence <=> o.production->dynamic_precedence; c != 0) return c;
    if (auto c = production->steps.size() <=> o.production->steps.size(); c != 0) return c;
    if (auto c = Prec() <=> o.Prec(); c != 0) return c;
    if (auto c = CmpOpt(Assoc(), o.Assoc()); c != 0) return c;
    for (size_t i = 0; i < production->steps.size(); i++) {
        const ProductionStep &a = production->steps[i], &b = o.production->steps[i];
        std::strong_ordering c = std::strong_ordering::equal;
        if (i < step_index) {
            c = CmpOpt(a.alias, b.alias);
            if (c == 0) c = CmpOpt(a.field_name, b.field_name);
        } else {
            c = a <=> b;
        }
        if (c != 0) return c;
    }
    return std::strong_ordering::equal;
}

static size_t Mix(size_t h, size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)); }

static size_t HashStep(const ProductionStep &s) {
    size_t h = SymbolHash()(s.symbol);
    h = Mix(h, static_cast<size_t>(s.precedence.kind) * 977 + static_cast<size_t>(s.precedence.integer));
    h = Mix(h, std::hash<std::string>()(s.precedence.name));
    h = Mix(h, s.associativity ? 1 + static_cast<size_t>(*s.associativity) : 0);
    if (s.alias) h = Mix(h, AliasHash()(*s.alias));
    if (s.field_name) h = Mix(h, std::hash<std::string>()(*s.field_name));
    return Mix(h, s.reserved_word_set_id);
}

size_t ParseItem::Hash() const {
    size_t h = variable_index;
    h = Mix(h, step_index);
    h = Mix(h, static_cast<size_t>(production->dynamic_precedence));
    h = Mix(h, production->steps.size());
    h = Mix(h, has_preceding_inherited_fields);
    const Precedence &p = Prec();
    h = Mix(h, static_cast<size_t>(p.kind) * 977 + static_cast<size_t>(p.integer));
    h = Mix(h, std::hash<std::string>()(p.name));
    auto a = Assoc();
    h = Mix(h, a ? 1 + static_cast<size_t>(*a) : 0);
    for (size_t i = 0; i < production->steps.size(); i++) {
        const ProductionStep &s = production->steps[i];
        if (i < step_index) {
            if (s.alias) h = Mix(h, AliasHash()(*s.alias));
            if (s.field_name) h = Mix(h, std::hash<std::string>()(*s.field_name));
            if (has_preceding_inherited_fields) h = Mix(h, SymbolHash()(s.symbol));
        } else {
            h = Mix(h, HashStep(s));
        }
    }
    return h;
}

ParseItemSetEntry &ParseItemSet::Insert(const ParseItem &item) {
    size_t lo = 0, hi = entries.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        auto c = entries[mid].item.Cmp(item);
        if (c == 0) return entries[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    entries.insert(entries.begin() + static_cast<long>(lo), ParseItemSetEntry{item, TokenSet{}, 0});
    return entries[lo];
}

size_t ParseItemSet::Hash() const {
    size_t h = entries.size();
    for (const ParseItemSetEntry &e : entries) {
        h = Mix(h, e.item.Hash());
        h = Mix(h, e.lookaheads.Hash());
        h = Mix(h, e.following_reserved_word_set);
    }
    return h;
}

size_t ParseItemSetCoreHash::operator()(const ParseItemSetCore &c) const {
    size_t h = c.entries.size();
    for (const ParseItem &i : c.entries) h = Mix(h, i.Hash());
    return h;
}

// ---- item_set_builder.rs -------------------------------------------------------

ParseItemSetBuilder::ParseItemSetBuilder(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                         const InlinedProductionMap &inlines)
    : syntax_(syntax), inlines_(inlines), transitive_closure_additions_(syntax.variables.size()) {
    for (size_t i = 0; i < lexical.variables.size(); i++) {
        Symbol s = Symbol::Terminal(i);
        TokenSet set;
        set.Insert(s);
        first_sets_[s] = set;
        last_sets_[s] = set;
        reserved_first_sets_[s] = 0;
    }
    for (size_t i = 0; i < syntax.external_tokens.size(); i++) {
        Symbol s = Symbol::External(i);
        TokenSet set;
        set.Insert(s);
        first_sets_[s] = set;
        last_sets_[s] = set;
        reserved_first_sets_[s] = 0;
    }
    std::vector<Symbol> to_process;
    std::set<Symbol> processed;
    for (size_t i = 0; i < syntax.variables.size(); i++) {
        Symbol symbol = Symbol::NonTerminal(i);
        TokenSet &first_set = first_sets_[symbol];
        size_t &reserved_first_set = reserved_first_sets_[symbol];
        processed.clear();
        to_process.clear();
        to_process.push_back(symbol);
        while (!to_process.empty()) {
            Symbol sym = to_process.back();
            to_process.pop_back();
            for (const Production &p : syntax.variables[sym.index].productions) {
                if (p.steps.empty()) continue;
                const ProductionStep &step = p.steps.front();
                if (step.symbol.IsTerminal() || step.symbol.IsExternal()) first_set.Insert(step.symbol);
                else if (processed.insert(step.symbol).second) to_process.push_back(step.symbol);
                reserved_first_set = std::max(reserved_first_set, step.reserved_word_set_id);
            }
        }
        TokenSet &last_set = last_sets_[symbol];
        processed.clear();
        to_process.clear();
        to_process.push_back(symbol);
        while (!to_process.empty()) {
            Symbol sym = to_process.back();
            to_process.pop_back();
            for (const Production &p : syntax.variables[sym.index].productions) {
                if (p.steps.empty()) continue;
                const ProductionStep &step = p.steps.back();
                if (step.symbol.IsTerminal() || step.symbol.IsExternal()) last_set.Insert(step.symbol);
                else if (processed.insert(step.symbol).second) to_process.push_back(step.symbol);
            }
        }
    }

    const TokenSet empty_lookaheads;
    struct StackEntry {
        size_t sym_ix;
        const TokenSet *lookaheads;
        size_t reserved_word_set_id;
        bool propagates_lookaheads;
    };
    std::vector<StackEntry> stack;
    std::map<size_t, FollowSetInfo> follow_set_info_by_non_terminal;
    for (size_t i = 0; i < syntax.variables.size(); i++) {
        stack.clear();
        stack.push_back({i, &empty_lookaheads, 0, true});
        follow_set_info_by_non_terminal.clear();
        while (!stack.empty()) {
            StackEntry e = stack.back();
            stack.pop_back();
            bool did_add = false;
            FollowSetInfo &info = follow_set_info_by_non_terminal[e.sym_ix];
            did_add |= info.lookaheads.InsertAll(*e.lookaheads);
            if (e.reserved_word_set_id > info.reserved_lookaheads) {
                info.reserved_lookaheads = e.reserved_word_set_id;
                did_add = true;
            }
            did_add |= e.propagates_lookaheads && !info.propagates_lookaheads;
            info.propagates_lookaheads |= e.propagates_lookaheads;
            if (!did_add) continue;
            for (const Production &p : syntax.variables[e.sym_ix].productions) {
                auto first = p.FirstSymbol();
                if (!first || !first->IsNonTerminal()) continue;
                if (p.steps.size() > 1) {
                    const Symbol &next = p.steps[1].symbol;
                    stack.push_back({first->index, &first_sets_.at(next), reserved_first_sets_.at(next), false});
                } else {
                    stack.push_back({first->index, e.lookaheads, e.reserved_word_set_id, e.propagates_lookaheads});
                }
            }
        }
        auto &additions = transitive_closure_additions_[i];
        for (const auto &[variable_index, follow_set_info] : follow_set_info_by_non_terminal) {
            const SyntaxVariable &variable = syntax.variables[variable_index];
            if (Contains(syntax.variables_to_inline, Symbol::NonTerminal(variable_index))) continue;
            for (const Production &production : variable.productions) {
                ParseItem item{static_cast<uint32_t>(variable_index), 0, &production, false};
                auto push = [&](const TransitiveClosureAddition &a) {
                    if (std::find(additions.begin(), additions.end(), a) == additions.end()) additions.push_back(a);
                };
                if (const auto *inlined = inlines.InlinedProductions(item.production, item.step_index)) {
                    for (size_t idx : *inlined) push({item.SubstituteProduction(inlines.productions[idx].get()), follow_set_info});
                } else {
                    push({item, follow_set_info});
                }
            }
        }
    }
}

const TokenSet *ParseItemSetBuilder::ReservedFirstSet(const Symbol &s) const {
    auto it = reserved_first_sets_.find(s);
    if (it == reserved_first_sets_.end()) return nullptr;
    return &syntax_.reserved_word_sets[it->second];
}

ParseItemSet ParseItemSetBuilder::TransitiveClosure(const ParseItemSet &item_set) const {
    ParseItemSet result;
    for (const ParseItemSetEntry &entry : item_set.entries) {
        if (const auto *productions = inlines_.InlinedProductions(entry.item.production, entry.item.step_index)) {
            for (size_t idx : *productions) {
                ParseItemSetEntry e{entry.item.SubstituteProduction(inlines_.productions[idx].get()), entry.lookaheads,
                                    entry.following_reserved_word_set};
                AddItem(result, e);
            }
        } else {
            AddItem(result, entry);
        }
    }
    return result;
}

void ParseItemSetBuilder::AddItem(ParseItemSet &set, const ParseItemSetEntry &entry) const {
    if (const ProductionStep *step = entry.item.Step()) {
        if (step->symbol.IsNonTerminal()) {
            ParseItem next = entry.item.Successor();
            const TokenSet *following_tokens;
            size_t following_reserved_tokens;
            if (const ProductionStep *next_step = next.Step()) {
                following_tokens = &first_sets_.at(next_step->symbol);
                following_reserved_tokens = reserved_first_sets_.at(next_step->symbol);
            } else {
                following_tokens = &entry.lookaheads;
                following_reserved_tokens = entry.following_reserved_word_set;
            }
            for (const TransitiveClosureAddition &addition : transitive_closure_additions_[step->symbol.index]) {
                ParseItemSetEntry &e = set.Insert(addition.item);
                e.lookaheads.InsertAll(addition.info.lookaheads);
                if (syntax_.word_token && addition.info.lookaheads.Contains(*syntax_.word_token))
                    e.following_reserved_word_set = std::max(e.following_reserved_word_set, addition.info.reserved_lookaheads);
                if (addition.info.propagates_lookaheads) {
                    e.lookaheads.InsertAll(*following_tokens);
                    if (syntax_.word_token && following_tokens->Contains(*syntax_.word_token))
                        e.following_reserved_word_set = std::max(e.following_reserved_word_set, following_reserved_tokens);
                }
            }
        }
    }
    ParseItemSetEntry &e = set.Insert(entry.item);
    e.lookaheads.InsertAll(entry.lookaheads);
    e.following_reserved_word_set = std::max(e.following_reserved_word_set, entry.following_reserved_word_set);
}

// ---- build_parse_table.rs ------------------------------------------------------

namespace {

struct AuxiliarySymbolInfo {
    Symbol auxiliary_symbol;
    std::vector<Symbol> parent_symbols;
    bool operator==(const AuxiliarySymbolInfo &) const = default;
};

struct ReductionInfo {
    Precedence precedence;
    std::vector<Symbol> symbols;
    bool has_left_assoc = false, has_right_assoc = false, has_non_assoc = false;
};

struct QueueEntry {
    size_t state_id;
    std::vector<AuxiliarySymbolInfo> preceding_auxiliary_symbols;
};

struct ItemPtrLess {
    bool operator()(const ParseItem *a, const ParseItem *b) const { return a->Cmp(*b) < 0; }
};

class ParseTableBuilder {
public:
    ParseTableBuilder(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const ParseItemSetBuilder &builder,
                      const std::vector<VariableInfo> &variable_info)
        : syntax_(syntax), lexical_(lexical), item_set_builder_(builder), variable_info_(variable_info) {
        for (const auto &c : syntax.expected_conflicts) actual_conflicts_.insert(c);
    }

    ParseTable Build() {
        table_.production_infos.push_back(ProductionInfo{});
        AddParseState({}, {}, ParseItemSet{});
        ParseItemSet start;
        TokenSet eof;
        eof.Insert(Symbol::End());
        start.entries.push_back({ParseItem::Start(), eof, 0});
        AddParseState({}, {}, start);

        std::map<Symbol, ParseItemSet> extra_item_sets_by_first_terminal;
        for (const Symbol &extra : syntax_.extra_symbols) {
            if (!extra.IsNonTerminal()) continue;
            const SyntaxVariable &variable = syntax_.variables[extra.index];
            for (const Production &production : variable.productions) {
                ParseItemSet &set = extra_item_sets_by_first_terminal[*production.FirstSymbol()];
                set.Insert(ParseItem{static_cast<uint32_t>(extra.index), 1, &production, false})
                    .lookaheads.Insert(Symbol::EndOfNonTerminalExtra());
            }
        }
        for (auto &[terminal, item_set] : extra_item_sets_by_first_terminal) {
            if (terminal.IsNonTerminal())
                Fail("The non-terminal rule `" + SymbolName(terminal) +
                     "` is used in a non-terminal `extra` rule, which is not allowed.");
            size_t state_id = AddParseState({}, {}, item_set);
            non_terminal_extra_states_.push_back({terminal, state_id});
        }
        while (!queue_.empty()) {
            QueueEntry entry = std::move(queue_.front());
            queue_.pop_front();
            ParseItemSet item_set = item_set_builder_.TransitiveClosure(state_info_[entry.state_id].item_set);
            AddActions(state_info_[entry.state_id].preceding_symbols, std::move(entry.preceding_auxiliary_symbols),
                       entry.state_id, item_set);
        }
        if (!actual_conflicts_.empty()) {
            std::printf("Warning: unnecessary conflicts\n");
            for (const auto &conflict : actual_conflicts_) {
                std::string line;
                for (size_t i = 0; i < conflict.size(); i++) line += (i ? ", `" : "`") + SymbolName(conflict[i]) + "`";
                std::printf("  %s\n", line.c_str());
            }
        }
        return std::move(table_);
    }

private:
    const SyntaxGrammar &syntax_;
    const LexicalGrammar &lexical_;
    const ParseItemSetBuilder &item_set_builder_;
    const std::vector<VariableInfo> &variable_info_;
    std::unordered_map<ParseItemSetCore, size_t, ParseItemSetCoreHash> core_ids_by_core_;
    std::unordered_map<ParseItemSet, size_t, ParseItemSetHash> state_ids_by_item_set_;
    std::vector<ParseStateInfo> state_info_;
    std::deque<QueueEntry> queue_;
    std::vector<std::pair<Symbol, size_t>> non_terminal_extra_states_;
    std::set<std::vector<Symbol>> actual_conflicts_;
    ParseTable table_;

    size_t AddParseState(const std::vector<Symbol> &preceding_symbols,
                         const std::vector<AuxiliarySymbolInfo> &preceding_auxiliary_symbols, ParseItemSet item_set) {
        auto it = state_ids_by_item_set_.find(item_set);
        if (it != state_ids_by_item_set_.end()) return it->second;
        ParseItemSetCore core;
        for (const ParseItemSetEntry &e : item_set.entries) core.entries.push_back(e.item);
        size_t core_count = core_ids_by_core_.size();
        size_t core_id = core_ids_by_core_.emplace(std::move(core), core_count).first->second;
        size_t state_id = table_.states.size();
        state_info_.push_back({preceding_symbols, item_set});
        ParseState state;
        state.id = state_id;
        state.core_id = core_id;
        table_.states.push_back(std::move(state));
        queue_.push_back({state_id, preceding_auxiliary_symbols});
        state_ids_by_item_set_.emplace(std::move(item_set), state_id);
        return state_id;
    }

    void AddActions(std::vector<Symbol> preceding_symbols, std::vector<AuxiliarySymbolInfo> preceding_auxiliary_symbols,
                    size_t state_id, const ParseItemSet &item_set) {
        std::map<Symbol, ParseItemSet> terminal_successors, non_terminal_successors;
        TokenSet lookaheads_with_conflicts;
        std::unordered_map<Symbol, ReductionInfo, SymbolHash> reduction_infos;
        for (const ParseItemSetEntry &entry : item_set.entries) {
            const ParseItem &item = entry.item;
            if (auto next_symbol = item.NextSymbol()) {
                ParseItem successor = item.Successor();
                ParseItemSet *successor_set;
                if (next_symbol->IsNonTerminal()) {
                    const SyntaxVariable &variable = syntax_.variables[next_symbol->index];
                    if (variable.IsAuxiliary()) preceding_auxiliary_symbols.push_back(GetAuxiliaryNodeInfo(item_set, *next_symbol));
                    if (variable.IsHidden() && !variable_info_[next_symbol->index].fields.empty())
                        successor.has_preceding_inherited_fields = true;
                    successor_set = &non_terminal_successors[*next_symbol];
                } else {
                    successor_set = &terminal_successors[*next_symbol];
                }
                ParseItemSetEntry &se = successor_set->Insert(successor);
                se.lookaheads.InsertAll(entry.lookaheads);
                se.following_reserved_word_set = std::max(se.following_reserved_word_set, entry.following_reserved_word_set);
            } else {
                Symbol symbol = Symbol::NonTerminal(item.variable_index);
                ParseAction action;
                if (item.IsAugmented()) {
                    action.kind = ParseAction::Kind::Accept;
                } else {
                    action.kind = ParseAction::Kind::Reduce;
                    action.symbol = symbol;
                    action.child_count = item.step_index;
                    action.dynamic_precedence = item.production->dynamic_precedence;
                    action.production_id = GetProductionId(item);
                }
                const Precedence &precedence = item.Prec();
                auto associativity = item.Assoc();
                for (const Symbol &lookahead : entry.lookaheads.Symbols()) {
                    ParseTableEntry &table_entry = table_.states[state_id].terminal_entries.OrInsert(lookahead, ParseTableEntry{});
                    ReductionInfo &reduction_info = reduction_infos[lookahead];
                    if (table_entry.actions.empty()) {
                        table_entry.actions.push_back(action);
                    } else {
                        int c = ComparePrecedence(precedence, {symbol}, reduction_info.precedence, reduction_info.symbols);
                        if (c > 0) {
                            table_entry.actions.clear();
                            table_entry.actions.push_back(action);
                            lookaheads_with_conflicts.Remove(lookahead);
                            reduction_info = ReductionInfo{};
                        } else if (c == 0) {
                            table_entry.actions.push_back(action);
                            lookaheads_with_conflicts.Insert(lookahead);
                        } else {
                            continue;
                        }
                    }
                    reduction_info.precedence = precedence;
                    auto pos = std::lower_bound(reduction_info.symbols.begin(), reduction_info.symbols.end(), symbol);
                    if (pos == reduction_info.symbols.end() || *pos != symbol) reduction_info.symbols.insert(pos, symbol);
                    if (!associativity) reduction_info.has_non_assoc = true;
                    else if (*associativity == Associativity::Left) reduction_info.has_left_assoc = true;
                    else reduction_info.has_right_assoc = true;
                }
            }
        }
        preceding_auxiliary_symbols.erase(std::unique(preceding_auxiliary_symbols.begin(), preceding_auxiliary_symbols.end()),
                                          preceding_auxiliary_symbols.end());
        for (auto &[symbol, next_item_set] : terminal_successors) {
            preceding_symbols.push_back(symbol);
            size_t next_state_id = AddParseState(preceding_symbols, preceding_auxiliary_symbols, std::move(next_item_set));
            preceding_symbols.pop_back();
            auto &entries = table_.states[state_id].terminal_entries;
            if (const ParseTableEntry *existing = entries.Get(symbol); existing && !existing->actions.empty())
                lookaheads_with_conflicts.Insert(symbol);
            ParseAction shift;
            shift.kind = ParseAction::Kind::Shift;
            shift.state = next_state_id;
            entries.OrInsert(symbol, ParseTableEntry{}).actions.push_back(shift);
        }
        for (auto &[symbol, next_item_set] : non_terminal_successors) {
            preceding_symbols.push_back(symbol);
            size_t next_state_id = AddParseState(preceding_symbols, preceding_auxiliary_symbols, std::move(next_item_set));
            preceding_symbols.pop_back();
            table_.states[state_id].nonterminal_entries.Insert(symbol, GotoAction{false, next_state_id});
        }
        for (const Symbol &symbol : lookaheads_with_conflicts.Symbols())
            HandleConflict(item_set, state_id, preceding_symbols, preceding_auxiliary_symbols, symbol, reduction_infos.at(symbol));

        ParseState &state = table_.states[state_id];
        if (state.IsEndOfNonTerminalExtra()) {
            if (state.terminal_entries.size() > 1) {
                std::set<uint32_t> parents;
                for (const ParseItemSetEntry &e : item_set.entries)
                    if (!e.item.IsAugmented() && e.item.step_index > 0) parents.insert(e.item.variable_index);
                std::string names;
                for (uint32_t p : parents) names += (names.empty() ? "" : ", ") + syntax_.variables[p].name;
                Fail("Extra rules must have unambiguous endings. Conflicting rules: " + names);
            }
        } else {
            for (const auto &[terminal, extra_state_id] : non_terminal_extra_states_) {
                ParseAction shift;
                shift.kind = ParseAction::Kind::Shift;
                shift.state = extra_state_id;
                state.terminal_entries.OrInsert(terminal, ParseTableEntry{{shift}, true});
            }
            for (const Symbol &extra : syntax_.extra_symbols) {
                if (extra.IsNonTerminal()) {
                    state.nonterminal_entries.Insert(extra, GotoAction{true, 0});
                } else {
                    ParseAction a;
                    a.kind = ParseAction::Kind::ShiftExtra;
                    state.terminal_entries.OrInsert(extra, ParseTableEntry{{a}, true});
                }
            }
        }
        if (syntax_.word_token) {
            std::optional<size_t> reserved_word_set_id;
            for (const ParseItemSetEntry &e : item_set.entries) {
                std::optional<size_t> id;
                if (const ProductionStep *next = e.item.Step()) {
                    if (next->symbol == *syntax_.word_token) id = next->reserved_word_set_id;
                } else if (e.lookaheads.Contains(*syntax_.word_token)) {
                    id = e.following_reserved_word_set;
                }
                if (id && (!reserved_word_set_id || *id > *reserved_word_set_id)) reserved_word_set_id = id;
            }
            if (reserved_word_set_id) state.reserved_words = syntax_.reserved_word_sets[*reserved_word_set_id];
        }
    }

    void HandleConflict(const ParseItemSet &item_set, size_t state_id, const std::vector<Symbol> &preceding_symbols,
                        const std::vector<AuxiliarySymbolInfo> &preceding_auxiliary_symbols, Symbol conflicting_lookahead,
                        const ReductionInfo &reduction_info) {
        ParseTableEntry *entry = table_.states[state_id].terminal_entries.Get(conflicting_lookahead);
        bool considered_associativity = false;
        std::vector<std::pair<const Precedence *, Symbol>> shift_precedence;
        std::set<const ParseItem *, ItemPtrLess> conflicting_items;
        for (const ParseItemSetEntry &e : item_set.entries) {
            const ParseItem &item = e.item;
            if (const ProductionStep *step = item.Step()) {
                if (item.step_index > 0 && item_set_builder_.FirstSet(step->symbol).Contains(conflicting_lookahead)) {
                    if (item.variable_index != UINT32_MAX) conflicting_items.insert(&item);
                    std::pair<const Precedence *, Symbol> p{&item.Prec(), Symbol::NonTerminal(item.variable_index)};
                    auto less = [](const std::pair<const Precedence *, Symbol> &a, const std::pair<const Precedence *, Symbol> &b) {
                        auto c = *a.first <=> *b.first;
                        if (c != 0) return c < 0;
                        return a.second < b.second;
                    };
                    auto pos = std::lower_bound(shift_precedence.begin(), shift_precedence.end(), p, less);
                    if (pos == shift_precedence.end() || less(p, *pos)) shift_precedence.insert(pos, p);
                }
            } else if (e.lookaheads.Contains(conflicting_lookahead) && item.variable_index != UINT32_MAX) {
                conflicting_items.insert(&item);
            }
        }
        if (entry->actions.back().kind == ParseAction::Kind::Shift) {
            uint32_t conflicting_variable_index = (*conflicting_items.begin())->variable_index;
            if (syntax_.variables[conflicting_variable_index].IsAuxiliary()) {
                bool all = true;
                for (const ParseItem *item : conflicting_items)
                    if (item->variable_index != conflicting_variable_index) all = false;
                if (all) {
                    entry->actions.back().is_repetition = true;
                    return;
                }
            }
            bool shift_is_less = false, shift_is_more = false;
            for (const auto &p : shift_precedence) {
                int c = ComparePrecedence(*p.first, {p.second}, reduction_info.precedence, reduction_info.symbols);
                if (c > 0) shift_is_more = true;
                else if (c < 0) shift_is_less = true;
            }
            auto drain_all_but_last = [&] { entry->actions.erase(entry->actions.begin(), entry->actions.end() - 1); };
            auto retain_done = [&] {
                for (auto it = conflicting_items.begin(); it != conflicting_items.end();) {
                    if (!(*it)->IsDone()) it = conflicting_items.erase(it);
                    else ++it;
                }
            };
            if (shift_is_more && !shift_is_less) {
                drain_all_but_last();
            } else if (shift_is_less && !shift_is_more) {
                entry->actions.pop_back();
                retain_done();
            } else if (!shift_is_less && !shift_is_more) {
                considered_associativity = true;
                if (reduction_info.has_left_assoc && !reduction_info.has_non_assoc && !reduction_info.has_right_assoc) {
                    entry->actions.pop_back();
                    retain_done();
                } else if (!reduction_info.has_left_assoc && !reduction_info.has_non_assoc && reduction_info.has_right_assoc) {
                    drain_all_but_last();
                }
            }
        }
        entry = table_.states[state_id].terminal_entries.Get(conflicting_lookahead);
        if (entry->actions.size() == 1) return;

        std::vector<Symbol> actual_conflict;
        for (const ParseItem *item : conflicting_items) {
            Symbol symbol = Symbol::NonTerminal(item->variable_index);
            if (syntax_.variables[symbol.index].IsAuxiliary()) {
                const AuxiliarySymbolInfo *found = nullptr;
                for (auto it = preceding_auxiliary_symbols.rbegin(); it != preceding_auxiliary_symbols.rend(); ++it)
                    if (it->auxiliary_symbol == symbol) {
                        found = &*it;
                        break;
                    }
                if (!found) Fail("internal: no parent symbols for an auxiliary symbol");
                actual_conflict.insert(actual_conflict.end(), found->parent_symbols.begin(), found->parent_symbols.end());
            } else {
                actual_conflict.push_back(symbol);
            }
        }
        std::sort(actual_conflict.begin(), actual_conflict.end());
        actual_conflict.erase(std::unique(actual_conflict.begin(), actual_conflict.end()), actual_conflict.end());
        for (const auto &expected : syntax_.expected_conflicts) {
            if (expected == actual_conflict) {
                actual_conflicts_.erase(actual_conflict);
                return;
            }
        }
        ReportConflict(preceding_symbols, conflicting_lookahead, conflicting_items, actual_conflict, considered_associativity);
    }

    [[noreturn]] void ReportConflict(const std::vector<Symbol> &preceding_symbols, Symbol conflicting_lookahead,
                                     const std::set<const ParseItem *, ItemPtrLess> &conflicting_items,
                                     const std::vector<Symbol> &actual_conflict, bool considered_associativity) {
        std::string out = "Unresolved conflict for symbol sequence:\n\n";
        for (const Symbol &s : preceding_symbols) out += "  " + SymbolName(s);
        std::string lookahead = SymbolName(conflicting_lookahead);
        out += "  •  " + lookahead + "  …\n\n";
        out += "Possible interpretations:\n\n";
        std::vector<std::pair<std::string, std::string>> interpretations;
        for (const ParseItem *item : conflicting_items) {
            std::string line;
            for (size_t i = 0; i + item->step_index < preceding_symbols.size(); i++) line += "  " + SymbolName(preceding_symbols[i]);
            line += "  (" + syntax_.variables[item->variable_index].name;
            for (size_t i = 0; i < item->production->steps.size(); i++) {
                if (i == item->step_index) line += "  •";
                line += "  " + SymbolName(item->production->steps[i].symbol);
            }
            line += ")";
            if (item->IsDone()) line += "  •  " + lookahead + "  …";
            std::string prec;
            const Precedence &p = item->Prec();
            auto assoc = item->Assoc();
            if (!p.IsNone() && assoc)
                prec = "(precedence: " + p.ToString() + ", associativity: " + (*assoc == Associativity::Left ? "Left" : "Right") + ")";
            else if (!p.IsNone())
                prec = "(precedence: " + p.ToString() + ")";
            interpretations.push_back({line, prec});
        }
        std::sort(interpretations.begin(), interpretations.end());
        for (size_t i = 0; i < interpretations.size(); i++) {
            out += "  " + std::to_string(i + 1) + ":" + interpretations[i].first;
            if (!interpretations[i].second.empty()) out += "  " + interpretations[i].second;
            out += "\n";
        }
        out += "\nPossible resolutions:\n\n";
        size_t n = 0;
        std::vector<const ParseItem *> shift_items, reduce_items;
        for (const ParseItem *item : conflicting_items) (item->IsDone() ? reduce_items : shift_items).push_back(item);
        auto rule_names = [&](const std::vector<const ParseItem *> &items) {
            std::vector<std::string> result;
            std::optional<uint32_t> last;
            for (const ParseItem *item : items) {
                if (last == item->variable_index) continue;
                last = item->variable_index;
                result.push_back("`" + syntax_.variables[item->variable_index].name + "`");
            }
            return result;
        };
        auto join = [](const std::vector<std::string> &v, const char *sep) {
            std::string s;
            for (size_t i = 0; i < v.size(); i++) s += (i ? sep : "") + v[i];
            return s;
        };
        if (actual_conflict.size() > 1) {
            if (!shift_items.empty())
                out += "  " + std::to_string(++n) + ":  Specify a higher precedence in " + join(rule_names(shift_items), " and ") +
                       " than in the other rules.\n";
            for (const ParseItem *item : reduce_items)
                out += "  " + std::to_string(++n) + ":  Specify a higher precedence in `" +
                       syntax_.variables[item->variable_index].name + "` than in the other rules.\n";
        }
        if (considered_associativity)
            out += "  " + std::to_string(++n) + ":  Specify a left or right associativity in " + join(rule_names(reduce_items), ", ") + "\n";
        std::vector<std::string> names;
        for (const Symbol &s : actual_conflict) names.push_back("`" + SymbolName(s) + "`");
        out += "  " + std::to_string(++n) + ":  Add a conflict for these rules: " + join(names, ", ") + "\n";
        Fail(out);
    }

    int ComparePrecedence(const Precedence &left, const std::vector<Symbol> &left_symbols, const Precedence &right,
                          const std::vector<Symbol> &right_symbols) const {
        using K = Precedence::Kind;
        auto cmp = [](int32_t a, int32_t b) { return a < b ? -1 : a > b ? 1 : 0; };
        if (left.kind == K::Integer && right.kind == K::Integer && (left.integer != 0 || right.integer != 0))
            return cmp(left.integer, right.integer);
        if (left.kind == K::Integer && right.kind == K::None && left.integer != 0) return cmp(left.integer, 0);
        if (left.kind == K::None && right.kind == K::Integer && right.integer != 0) return cmp(0, right.integer);
        auto matches = [&](const PrecedenceEntry &entry, const Precedence &precedence, const std::vector<Symbol> &symbols) {
            if (entry.kind == PrecedenceEntry::Kind::Name) return precedence.kind == K::Name && precedence.name == entry.value;
            for (const Symbol &s : symbols)
                if (syntax_.variables[s.index].name == entry.value) return true;
            return false;
        };
        for (const auto &list : syntax_.precedence_orderings) {
            bool saw_left = false, saw_right = false;
            for (const PrecedenceEntry &entry : list) {
                bool ml = matches(entry, left, left_symbols);
                bool mr = matches(entry, right, right_symbols);
                if (ml) {
                    saw_left = true;
                    if (saw_right) return -1;
                } else if (mr) {
                    saw_right = true;
                    if (saw_left) return 1;
                }
            }
        }
        return 0;
    }

    AuxiliarySymbolInfo GetAuxiliaryNodeInfo(const ParseItemSet &item_set, Symbol symbol) const {
        AuxiliarySymbolInfo info{symbol, {}};
        for (const ParseItemSetEntry &e : item_set.entries) {
            size_t vi = e.item.variable_index;
            if (e.item.NextSymbol() == std::optional<Symbol>(symbol) && !syntax_.variables[vi].IsAuxiliary())
                info.parent_symbols.push_back(Symbol::NonTerminal(vi));
        }
        return info;
    }

    size_t GetProductionId(const ParseItem &item) {
        ProductionInfo info;
        for (size_t i = 0; i < item.production->steps.size(); i++) {
            const ProductionStep &step = item.production->steps[i];
            info.alias_sequence.push_back(step.alias);
            if (step.field_name) info.field_map[*step.field_name].push_back({i, false});
            if (step.symbol.kind == SymbolType::NonTerminal && !IsVisible(syntax_.variables[step.symbol.index].kind)) {
                for (const auto &[field_name, f] : variable_info_[step.symbol.index].fields)
                    info.field_map[field_name].push_back({i, true});
            }
        }
        while (!info.alias_sequence.empty() && !info.alias_sequence.back()) info.alias_sequence.pop_back();
        if (item.production->steps.size() > table_.max_aliased_production_length)
            table_.max_aliased_production_length = item.production->steps.size();
        for (size_t k = 0; k < table_.production_infos.size(); k++)
            if (table_.production_infos[k] == info) return k;
        table_.production_infos.push_back(std::move(info));
        return table_.production_infos.size() - 1;
    }

    std::string SymbolName(const Symbol &s) const {
        switch (s.kind) {
            case SymbolType::End:
            case SymbolType::EndOfNonTerminalExtra:
                return "EOF";
            case SymbolType::External:
                return syntax_.external_tokens[s.index].name;
            case SymbolType::NonTerminal:
                return syntax_.variables[s.index].name;
            case SymbolType::Terminal: {
                const LexicalVariable &v = lexical_.variables[s.index];
                return v.kind == VariableType::Named ? v.name : "'" + v.name + "'";
            }
        }
        return "";
    }
};

}  // namespace

ParseTable BuildParseTable(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const ParseItemSetBuilder &builder,
                           const std::vector<VariableInfo> &variable_info) {
    return ParseTableBuilder(syntax, lexical, builder, variable_info).Build();
}

}  // namespace tsgen
