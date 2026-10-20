// mep-ts-generate's pipeline: grammar.json -> parser.c and node-types.json,
// as tree-sitter 0.25.10's `tree-sitter generate` does (cli/generate/src):
//
//   ParseGrammar        parse_grammar.rs
//   PrepareGrammar      prepare_grammar/*.rs
//   GetVariableInfo,    node_types.rs
//   GenerateNodeTypesJson
//   BuildTables         build_tables/*.rs
//   RenderC             render.rs
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"
#include "json.h"

namespace tsgen {

// grammar_js.cpp: runs `grammar.js` (and the modules it requires) with
// dsl.js on mep's JS engine, as the CLI runs it on Node; returns the
// grammar.json text.
std::string EvaluateGrammarJs(const std::filesystem::path &grammar_js);

InputGrammar ParseGrammar(const JsonValue &json);

struct PreparedGrammar {
    SyntaxGrammar syntax;
    LexicalGrammar lexical;
    InlinedProductionMap inlines;
    AliasMap simple_aliases;
};

PreparedGrammar PrepareGrammar(const InputGrammar &input);

// ---- node_types.rs ------------------------------------------------------------

// enum ChildType { Normal(Symbol), Aliased(Alias) }
struct ChildType {
    bool aliased = false;
    Symbol symbol;
    Alias alias;
    std::strong_ordering operator<=>(const ChildType &o) const {
        if (aliased != o.aliased) return aliased <=> o.aliased;
        return aliased ? alias <=> o.alias : symbol <=> o.symbol;
    }
    bool operator==(const ChildType &o) const { return (*this <=> o) == 0; }
};

struct ChildQuantity {
    bool exists = true, required = true, multiple = false;
    static ChildQuantity Zero() { return {false, false, false}; }
    static ChildQuantity One() { return {true, true, false}; }
    void Append(ChildQuantity other);
    bool Union(ChildQuantity other);
    bool operator==(const ChildQuantity &) const = default;
};

struct FieldInfo {
    ChildQuantity quantity;
    std::vector<ChildType> types;
    bool operator==(const FieldInfo &) const = default;
};

struct VariableInfo {
    std::map<std::string, FieldInfo> fields;  // a HashMap in Rust: only looked up, never ordered
    FieldInfo children;
    FieldInfo children_without_fields;
    bool has_multi_step_production = false;
};

std::vector<VariableInfo> GetVariableInfo(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                          const AliasMap &default_aliases);
JsonValue GenerateNodeTypesJson(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                const AliasMap &default_aliases, const std::vector<VariableInfo> &variable_info);
std::map<Symbol, std::vector<ChildType>> GetSupertypeSymbolMap(const SyntaxGrammar &syntax,
                                                               const AliasMap &default_aliases,
                                                               const std::vector<VariableInfo> &variable_info);

// ---- tables.rs ----------------------------------------------------------------

// enum ParseAction { Accept, Shift{..}, ShiftExtra, Recover, Reduce{..} }
struct ParseAction {
    enum class Kind : uint8_t { Accept, Shift, ShiftExtra, Recover, Reduce } kind = Kind::Accept;
    size_t state = 0;  // Shift
    bool is_repetition = false;
    Symbol symbol;  // Reduce
    size_t child_count = 0;
    int32_t dynamic_precedence = 0;
    size_t production_id = 0;

    bool operator==(const ParseAction &o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Shift) return state == o.state && is_repetition == o.is_repetition;
        if (kind == Kind::Reduce)
            return symbol == o.symbol && child_count == o.child_count && dynamic_precedence == o.dynamic_precedence &&
                   production_id == o.production_id;
        return true;
    }
    std::strong_ordering operator<=>(const ParseAction &o) const;
};

struct GotoAction {
    bool shift_extra = false;  // GotoAction::ShiftExtra, else Goto(state)
    size_t state = 0;
    bool operator==(const GotoAction &) const = default;
};

struct ParseTableEntry {
    std::vector<ParseAction> actions;
    bool reusable = true;
    bool operator==(const ParseTableEntry &) const = default;
};

// indexmap::IndexMap: insertion order; `insert` on an existing key keeps
// its place; equality ignores order.
template <class K, class V, class H>
class IndexMap {
public:
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    V *Get(const K &k) {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &entries_[it->second].second;
    }
    const V *Get(const K &k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &entries_[it->second].second;
    }
    bool Contains(const K &k) const { return index_.count(k) != 0; }
    // entry(k).or_insert(v)
    V &OrInsert(const K &k, V v) {
        auto it = index_.find(k);
        if (it != index_.end()) return entries_[it->second].second;
        index_[k] = entries_.size();
        entries_.push_back({k, std::move(v)});
        return entries_.back().second;
    }
    void Insert(const K &k, V v) {
        auto it = index_.find(k);
        if (it != index_.end()) {
            entries_[it->second].second = std::move(v);
            return;
        }
        index_[k] = entries_.size();
        entries_.push_back({k, std::move(v)});
    }
    void Extend(const IndexMap &o) {
        for (const auto &kv : o.entries_) Insert(kv.first, kv.second);
    }
    std::vector<std::pair<K, V>> &Entries() { return entries_; }
    const std::vector<std::pair<K, V>> &Entries() const { return entries_; }
    bool operator==(const IndexMap &o) const {
        if (size() != o.size()) return false;
        for (const auto &kv : entries_) {
            const V *v = o.Get(kv.first);
            if (!v || !(*v == kv.second)) return false;
        }
        return true;
    }

private:
    std::vector<std::pair<K, V>> entries_;
    std::unordered_map<K, size_t, H> index_;
};

struct ParseState {
    size_t id = 0;
    IndexMap<Symbol, ParseTableEntry, SymbolHash> terminal_entries;
    IndexMap<Symbol, GotoAction, SymbolHash> nonterminal_entries;
    TokenSet reserved_words;
    size_t lex_state_id = 0;
    size_t external_lex_state_id = 0;
    size_t core_id = 0;

    bool IsEndOfNonTerminalExtra() const { return terminal_entries.Contains(Symbol::EndOfNonTerminalExtra()); }
    std::vector<size_t> ReferencedStates() const;
    template <class F>
    void UpdateReferencedStates(F f);
};

struct FieldLocation {
    size_t index = 0;
    bool inherited = false;
    bool operator==(const FieldLocation &) const = default;
};

struct ProductionInfo {
    std::vector<std::optional<Alias>> alias_sequence;
    std::map<std::string, std::vector<FieldLocation>> field_map;
    bool operator==(const ProductionInfo &) const = default;
};

struct ParseTable {
    std::vector<ParseState> states;
    std::vector<Symbol> symbols;
    std::vector<ProductionInfo> production_infos;
    size_t max_aliased_production_length = 1;
    std::vector<TokenSet> external_lex_states;
};

struct AdvanceAction {
    size_t state = 0;
    bool in_main_token = false;
    auto operator<=>(const AdvanceAction &) const = default;
    bool operator==(const AdvanceAction &) const = default;
};

struct LexState {
    std::optional<Symbol> accept_action;
    std::optional<AdvanceAction> eof_action;
    std::vector<std::pair<CharacterSet, AdvanceAction>> advance_actions;
    bool operator==(const LexState &) const = default;
    std::strong_ordering operator<=>(const LexState &o) const;
};

struct LexTable {
    std::vector<LexState> states;
};

struct Tables {
    ParseTable parse_table;
    LexTable main_lex_table;
    LexTable keyword_lex_table;
    std::vector<std::pair<std::optional<Symbol>, CharacterSet>> large_character_sets;
};

Tables BuildTables(const SyntaxGrammar &syntax, const LexicalGrammar &lexical, const AliasMap &simple_aliases,
                   const std::vector<VariableInfo> &variable_info, const InlinedProductionMap &inlines);

// dedup.rs's split_state_id_groups
template <class S, class F>
bool SplitStateIdGroups(const std::vector<S> &states, std::vector<std::vector<size_t>> &state_ids_by_group_id,
                        std::vector<size_t> &group_ids_by_state_id, size_t start_group_id, F should_split) {
    bool result = false;
    for (size_t group_id = start_group_id; group_id < state_ids_by_group_id.size(); group_id++) {
        const std::vector<size_t> &state_ids = state_ids_by_group_id[group_id];
        std::vector<size_t> split_state_ids;
        auto is_split = [&](size_t id) {
            for (size_t s : split_state_ids)
                if (s == id) return true;
            return false;
        };
        for (size_t i = 0; i < state_ids.size(); i++) {
            size_t left_state_id = state_ids[i];
            if (is_split(left_state_id)) continue;
            const S &left_state = states[left_state_id];
            for (size_t j = i + 1; j < state_ids.size(); j++) {
                size_t right_state_id = state_ids[j];
                if (is_split(right_state_id)) continue;
                if (should_split(left_state, states[right_state_id], group_ids_by_state_id))
                    split_state_ids.push_back(right_state_id);
            }
        }
        if (!split_state_ids.empty()) {
            result = true;
            std::vector<size_t> kept;
            for (size_t id : state_ids_by_group_id[group_id])
                if (!is_split(id)) kept.push_back(id);
            state_ids_by_group_id[group_id] = std::move(kept);
            size_t new_group_id = state_ids_by_group_id.size();
            for (size_t id : split_state_ids) group_ids_by_state_id[id] = new_group_id;
            state_ids_by_group_id.push_back(std::move(split_state_ids));
        }
    }
    return result;
}

template <class F>
void ParseState::UpdateReferencedStates(F f) {
    struct Update {
        Symbol symbol;
        size_t action_index, new_state;
    };
    std::vector<Update> updates;
    for (const auto &[symbol, entry] : terminal_entries.Entries()) {
        for (size_t i = 0; i < entry.actions.size(); i++) {
            const ParseAction &action = entry.actions[i];
            if (action.kind == ParseAction::Kind::Shift) {
                size_t result = f(action.state, *this);
                if (result != action.state) updates.push_back({symbol, i, result});
            }
        }
    }
    for (const auto &[symbol, action] : nonterminal_entries.Entries()) {
        if (!action.shift_extra) {
            size_t result = f(action.state, *this);
            if (result != action.state) updates.push_back({symbol, 0, result});
        }
    }
    for (const Update &u : updates) {
        if (u.symbol.IsNonTerminal()) {
            nonterminal_entries.Insert(u.symbol, GotoAction{false, u.new_state});
        } else {
            ParseTableEntry *entry = terminal_entries.Get(u.symbol);
            if (entry->actions[u.action_index].kind == ParseAction::Kind::Shift) entry->actions[u.action_index].state = u.new_state;
        }
    }
}

// ---- render.rs ----------------------------------------------------------------

struct SemanticVersion {
    uint8_t major = 0, minor = 0, patch = 0;
};

std::string RenderC(const std::string &name, Tables tables, SyntaxGrammar syntax, LexicalGrammar lexical,
                    AliasMap default_aliases, size_t abi_version, std::optional<SemanticVersion> semantic_version,
                    std::map<Symbol, std::vector<ChildType>> supertype_symbol_map);

}  // namespace tsgen
