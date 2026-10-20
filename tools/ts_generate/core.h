// mep-ts-generate's data model: a port of tree-sitter 0.25.10's
// cli/generate/src/{rules,grammars,nfa}.rs types.
//
// The point of the port is byte-identical output, so the types keep the
// Rust originals' semantics exactly, including the incidental ones:
// derived orderings follow declaration order (an enum's variants, a
// struct's fields, `None` before `Some`), and a TokenSet's bit vectors keep
// their length -- two sets that differ only in trailing unset bits are
// unequal and order differently, as smallbitvec's are, which decides which
// parse states are merged.
#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tsgen {

[[noreturn]] void Fail(const std::string &message);

// ---- rules.rs ------------------------------------------------------------

enum class SymbolType : uint8_t { External, End, EndOfNonTerminalExtra, Terminal, NonTerminal };

struct Symbol {
    SymbolType kind = SymbolType::Terminal;
    size_t index = 0;

    static Symbol Terminal(size_t i) { return {SymbolType::Terminal, i}; }
    static Symbol NonTerminal(size_t i) { return {SymbolType::NonTerminal, i}; }
    static Symbol External(size_t i) { return {SymbolType::External, i}; }
    static Symbol End() { return {SymbolType::End, 0}; }
    static Symbol EndOfNonTerminalExtra() { return {SymbolType::EndOfNonTerminalExtra, 0}; }
    bool IsTerminal() const { return kind == SymbolType::Terminal; }
    bool IsNonTerminal() const { return kind == SymbolType::NonTerminal; }
    bool IsExternal() const { return kind == SymbolType::External; }
    bool IsEof() const { return kind == SymbolType::End; }
    auto operator<=>(const Symbol &) const = default;
    bool operator==(const Symbol &) const = default;
};

struct SymbolHash {
    size_t operator()(const Symbol &s) const { return s.index * 8 + static_cast<size_t>(s.kind); }
};

enum class Associativity : uint8_t { Left, Right };

enum class VariableType : uint8_t { Hidden, Auxiliary, Anonymous, Named };
inline bool IsVisible(VariableType t) { return t == VariableType::Named || t == VariableType::Anonymous; }

struct Alias {
    std::string value;
    bool is_named = false;
    VariableType Kind() const { return is_named ? VariableType::Named : VariableType::Anonymous; }
    auto operator<=>(const Alias &) const = default;
    bool operator==(const Alias &) const = default;
};

struct AliasHash {
    size_t operator()(const Alias &a) const { return std::hash<std::string>()(a.value) * 2 + a.is_named; }
};

// enum Precedence { None, Integer(i32), Name(String) }
struct Precedence {
    enum class Kind : uint8_t { None, Integer, Name } kind = Kind::None;
    int32_t integer = 0;
    std::string name;

    static Precedence Int(int32_t v) { return {Kind::Integer, v, {}}; }
    static Precedence Named(std::string n) { return {Kind::Name, 0, std::move(n)}; }
    bool IsNone() const { return kind == Kind::None; }
    std::strong_ordering operator<=>(const Precedence &o) const {
        if (kind != o.kind) return kind <=> o.kind;
        if (kind == Kind::Integer) return integer <=> o.integer;
        if (kind == Kind::Name) return name <=> o.name;
        return std::strong_ordering::equal;
    }
    bool operator==(const Precedence &o) const { return (*this <=> o) == 0; }
    std::string ToString() const;  // Display: `3`, `'name'`, `none`
};

struct MetadataParams {
    Precedence precedence;
    int32_t dynamic_precedence = 0;
    std::optional<Associativity> associativity;
    bool is_token = false;
    bool is_main_token = false;
    std::optional<Alias> alias;
    std::optional<std::string> field_name;
    bool operator==(const MetadataParams &) const = default;
};

struct Rule {
    enum class Kind : uint8_t { Blank, String, Pattern, NamedSymbol, Symbol, Choice, Metadata, Repeat, Seq, Reserved };
    Kind kind = Kind::Blank;
    std::string value;  // String, Pattern, NamedSymbol; Reserved's context name
    std::string flags;  // Pattern
    tsgen::Symbol symbol;
    std::vector<Rule> members;  // Choice/Seq; the one inner rule of Metadata/Repeat/Reserved
    std::shared_ptr<const MetadataParams> params;  // Metadata

    static Rule Blank() { return {}; }
    static Rule String(std::string s);
    static Rule Pattern(std::string s, std::string f);
    static Rule Named(std::string s);
    static Rule Sym(tsgen::Symbol s);
    static Rule SeqOf(std::vector<Rule> rules);
    // Rule::choice: flattens nested choices and drops duplicates.
    static Rule ChoiceOf(std::vector<Rule> rules);
    static Rule RepeatOf(Rule r);
    static Rule MetadataOf(Rule r, MetadataParams p);
    static Rule ReservedOf(Rule r, std::string context);

    const Rule &Inner() const { return members[0]; }
    const MetadataParams &Params() const { return *params; }
    bool IsEmpty() const;
    bool operator==(const Rule &o) const;
};

struct RuleHash {
    size_t operator()(const Rule &r) const;
};

// rules.rs's add_metadata: applies `f` to the params of a Metadata rule
// that is not a token, else wraps the rule in fresh params.
template <class F>
Rule AddMetadata(Rule input, F f) {
    if (input.kind == Rule::Kind::Metadata && !input.Params().is_token) {
        MetadataParams p = input.Params();
        f(p);
        input.params = std::make_shared<const MetadataParams>(std::move(p));
        return input;
    }
    MetadataParams p;
    f(p);
    return Rule::MetadataOf(std::move(input), std::move(p));
}

// A smallbitvec::SmallBitVec, as TokenSet uses it: a bit vector whose
// length is part of its value.
using BitVec = std::vector<bool>;

int CompareBitVec(const BitVec &a, const BitVec &b);  // lexicographic over the bits

struct TokenSet {
    BitVec terminal_bits;
    BitVec external_bits;
    bool eof = false;
    bool end_of_nonterminal_extra = false;

    bool operator==(const TokenSet &) const = default;
    std::strong_ordering operator<=>(const TokenSet &o) const;
    std::vector<Symbol> Symbols() const;  // iter(): terminals, externals, end, end-of-extra
    std::vector<Symbol> Terminals() const;
    bool Contains(const Symbol &s) const;
    bool ContainsTerminal(size_t i) const { return i < terminal_bits.size() && terminal_bits[i]; }
    void Insert(const Symbol &s);
    bool Remove(const Symbol &s);
    bool IsEmpty() const;
    size_t Len() const;
    bool InsertAllTerminals(const TokenSet &o);
    bool InsertAllExternals(const TokenSet &o);
    bool InsertAll(const TokenSet &o);
    template <class It>
    static TokenSet From(const It &symbols) {
        TokenSet t;
        for (const Symbol &s : symbols) t.Insert(s);
        return t;
    }
    size_t Hash() const;
};

struct TokenSetHash {
    size_t operator()(const TokenSet &t) const { return t.Hash(); }
};

// ---- nfa.rs --------------------------------------------------------------

constexpr uint32_t kCharEnd = 0x110000;  // char::MAX + 1

// A set of characters as sorted, disjoint, non-adjacent half-open ranges.
struct CharacterSet {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;

    static CharacterSet FromChar(uint32_t c) { return CharacterSet{{{c, c + 1}}}; }
    CharacterSet Negate() const;
    CharacterSet AddChar(uint32_t c) const;
    CharacterSet AddRange(uint32_t start, uint32_t end_inclusive) const;  // a char range
    CharacterSet Add(const CharacterSet &o) const;
    size_t AddIntRange(size_t i, uint32_t start, uint32_t end);
    bool DoesIntersect(const CharacterSet &o) const;
    CharacterSet RemoveIntersection(CharacterSet &o);
    CharacterSet Difference(CharacterSet o) const;
    size_t RangeCount() const { return ranges.size(); }
    // ranges(): each range's first and last valid char (surrogates skipped)
    std::vector<std::pair<uint32_t, uint32_t>> CharRanges() const;
    bool IsEmpty() const { return ranges.empty(); }
    CharacterSet SimplifyIgnoring(const CharacterSet &ruled_out) const;
    bool ContainsCodepointRange(uint32_t start, uint32_t end) const;
    bool Contains(uint32_t c) const { return ContainsCodepointRange(c, c + 1); }
    bool operator==(const CharacterSet &) const = default;
    std::strong_ordering operator<=>(const CharacterSet &o) const;  // nfa.rs's Ord
    size_t Hash() const;
};

struct CharacterSetHash {
    size_t operator()(const CharacterSet &c) const { return c.Hash(); }
};

struct NfaState {
    enum class Kind : uint8_t { Advance, Split, Accept } kind = Kind::Accept;
    CharacterSet chars;     // Advance
    uint32_t state_id = 0;  // Advance; Split's left
    uint32_t right = 0;     // Split's right
    bool is_sep = false;    // Advance
    int32_t precedence = 0;  // Advance, Accept
    size_t variable_index = 0;  // Accept
};

struct Nfa {
    std::vector<NfaState> states;
    uint32_t LastStateId() const { return static_cast<uint32_t>(states.size()) - 1; }
};

struct NfaTransition {
    CharacterSet characters;
    bool is_separator = false;
    int32_t precedence = 0;
    std::vector<uint32_t> states;
};

class NfaCursor {
public:
    explicit NfaCursor(const Nfa *nfa) : nfa_(nfa) {}
    void Reset(std::vector<uint32_t> states);
    void ForceReset(std::vector<uint32_t> states) { state_ids = std::move(states); }
    // transition_chars(): (chars, is_sep) of each Advance state
    std::vector<std::pair<const CharacterSet *, bool>> TransitionChars() const;
    std::vector<NfaTransition> Transitions() const;
    std::vector<std::pair<size_t, int32_t>> Completions() const;
    void AddStates(std::vector<uint32_t> &new_state_ids);

    std::vector<uint32_t> state_ids;

private:
    const Nfa *nfa_;
};

// ---- grammars.rs ---------------------------------------------------------

struct Variable {
    std::string name;
    VariableType kind = VariableType::Named;
    Rule rule;
    bool operator==(const Variable &) const = default;
};

struct PrecedenceEntry {
    enum class Kind : uint8_t { Name, Symbol } kind = Kind::Name;
    std::string value;
    auto operator<=>(const PrecedenceEntry &) const = default;
    bool operator==(const PrecedenceEntry &) const = default;
    std::string ToString() const { return kind == Kind::Name ? "'" + value + "'" : "$." + value; }
};

template <class T>
struct ReservedWordContext {
    std::string name;
    std::vector<T> reserved_words;
};

struct InputGrammar {
    std::string name;
    std::vector<Variable> variables;
    std::vector<Rule> extra_symbols;
    std::vector<std::vector<std::string>> expected_conflicts;
    std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
    std::vector<Rule> external_tokens;
    std::vector<std::string> variables_to_inline;
    std::vector<std::string> supertype_symbols;
    std::optional<std::string> word_token;
    std::vector<ReservedWordContext<Rule>> reserved_words;
};

struct LexicalVariable {
    std::string name;
    VariableType kind = VariableType::Named;
    int32_t implicit_precedence = 0;
    uint32_t start_state = 0;
};

struct LexicalGrammar {
    Nfa nfa;
    std::vector<LexicalVariable> variables;
    // variable_index_for_nfa_state: the first variable whose start state is >= the state
    size_t VariableIndexForNfaState(uint32_t state_id) const;
    // variable_indices_for_nfa_states: the distinct consecutive variable indices
    std::vector<size_t> VariableIndicesForNfaStates(const std::vector<uint32_t> &state_ids) const;
};

constexpr size_t kNoReservedWords = SIZE_MAX;  // NO_RESERVED_WORDS

struct ProductionStep {
    Symbol symbol;
    Precedence precedence;
    std::optional<Associativity> associativity;
    std::optional<Alias> alias;
    std::optional<std::string> field_name;
    size_t reserved_word_set_id = 0;
    bool operator==(const ProductionStep &) const = default;
    std::strong_ordering operator<=>(const ProductionStep &o) const;
};

struct Production {
    std::vector<ProductionStep> steps;
    int32_t dynamic_precedence = 0;
    bool operator==(const Production &) const = default;
    std::optional<Symbol> FirstSymbol() const {
        if (steps.empty()) return std::nullopt;
        return steps[0].symbol;
    }
};

struct SyntaxVariable {
    std::string name;
    VariableType kind = VariableType::Named;
    std::vector<Production> productions;
    bool IsAuxiliary() const { return kind == VariableType::Auxiliary; }
    bool IsHidden() const { return kind == VariableType::Hidden || kind == VariableType::Auxiliary; }
};

struct ExternalToken {
    std::string name;
    VariableType kind = VariableType::Named;
    std::optional<Symbol> corresponding_internal_token;
};

struct SyntaxGrammar {
    std::vector<SyntaxVariable> variables;
    std::vector<Symbol> extra_symbols;
    std::vector<std::vector<Symbol>> expected_conflicts;
    std::vector<ExternalToken> external_tokens;
    std::vector<Symbol> supertype_symbols;
    std::vector<Symbol> variables_to_inline;
    std::optional<Symbol> word_token;
    std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
    std::vector<TokenSet> reserved_word_sets;
};

struct InlinedProductionMap {
    // Stable addresses: productions are referred to by pointer.
    std::vector<std::unique_ptr<Production>> productions;
    struct KeyHash {
        size_t operator()(const std::pair<const Production *, uint32_t> &k) const {
            return std::hash<const void *>()(k.first) * 31 + k.second;
        }
    };
    std::unordered_map<std::pair<const Production *, uint32_t>, std::vector<size_t>, KeyHash> production_map;
    const std::vector<size_t> *InlinedProductions(const Production *production, uint32_t step_index) const {
        auto it = production_map.find({production, step_index});
        return it == production_map.end() ? nullptr : &it->second;
    }
};

using AliasMap = std::unordered_map<Symbol, Alias, SymbolHash>;

inline bool Contains(const std::vector<Symbol> &v, const Symbol &s) {
    for (const Symbol &x : v)
        if (x == s) return true;
    return false;
}

}  // namespace tsgen
