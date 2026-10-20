// mep-ts-generate's data model: see core.h.
#include "core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace tsgen {

void Fail(const std::string &message) {
    std::fprintf(stderr, "mep-ts-generate: %s\n", message.c_str());
    std::exit(1);
}

std::string Precedence::ToString() const {
    switch (kind) {
        case Kind::Integer:
            return std::to_string(integer);
        case Kind::Name:
            return "'" + name + "'";
        default:
            return "none";
    }
}

// ---- Rule -----------------------------------------------------------------

Rule Rule::String(std::string s) {
    Rule r;
    r.kind = Kind::String;
    r.value = std::move(s);
    return r;
}

Rule Rule::Pattern(std::string s, std::string f) {
    Rule r;
    r.kind = Kind::Pattern;
    r.value = std::move(s);
    r.flags = std::move(f);
    return r;
}

Rule Rule::Named(std::string s) {
    Rule r;
    r.kind = Kind::NamedSymbol;
    r.value = std::move(s);
    return r;
}

Rule Rule::Sym(tsgen::Symbol s) {
    Rule r;
    r.kind = Kind::Symbol;
    r.symbol = s;
    return r;
}

Rule Rule::SeqOf(std::vector<Rule> rules) {
    Rule r;
    r.kind = Kind::Seq;
    r.members = std::move(rules);
    return r;
}

static void ChoiceHelper(std::vector<Rule> &result, Rule rule) {
    if (rule.kind == Rule::Kind::Choice) {
        for (Rule &element : rule.members) ChoiceHelper(result, std::move(element));
        return;
    }
    for (const Rule &existing : result)
        if (existing == rule) return;
    result.push_back(std::move(rule));
}

Rule Rule::ChoiceOf(std::vector<Rule> rules) {
    Rule r;
    r.kind = Kind::Choice;
    for (Rule &rule : rules) ChoiceHelper(r.members, std::move(rule));
    return r;
}

Rule Rule::RepeatOf(Rule inner) {
    Rule r;
    r.kind = Kind::Repeat;
    r.members.push_back(std::move(inner));
    return r;
}

Rule Rule::MetadataOf(Rule inner, MetadataParams p) {
    Rule r;
    r.kind = Kind::Metadata;
    r.members.push_back(std::move(inner));
    r.params = std::make_shared<const MetadataParams>(std::move(p));
    return r;
}

Rule Rule::ReservedOf(Rule inner, std::string context) {
    Rule r;
    r.kind = Kind::Reserved;
    r.value = std::move(context);
    r.members.push_back(std::move(inner));
    return r;
}

bool Rule::IsEmpty() const {
    switch (kind) {
        case Kind::Blank:
        case Kind::Pattern:
        case Kind::NamedSymbol:
        case Kind::Symbol:
            return false;
        case Kind::String:
            return value.empty();
        case Kind::Metadata:
        case Kind::Repeat:
        case Kind::Reserved:
            return Inner().IsEmpty();
        case Kind::Choice:
            for (const Rule &m : members)
                if (m.IsEmpty()) return true;
            return false;
        case Kind::Seq:
            for (const Rule &m : members)
                if (!m.IsEmpty()) return false;
            return true;
    }
    return false;
}

bool Rule::operator==(const Rule &o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Blank:
            return true;
        case Kind::String:
        case Kind::NamedSymbol:
            return value == o.value;
        case Kind::Pattern:
            return value == o.value && flags == o.flags;
        case Kind::Symbol:
            return symbol == o.symbol;
        case Kind::Choice:
        case Kind::Seq:
        case Kind::Repeat:
            return members == o.members;
        case Kind::Metadata:
            return (params == o.params || *params == *o.params) && members == o.members;
        case Kind::Reserved:
            return value == o.value && members == o.members;
    }
    return false;
}

static size_t Mix(size_t h, size_t v) { return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)); }

size_t RuleHash::operator()(const Rule &r) const {
    size_t h = static_cast<size_t>(r.kind);
    h = Mix(h, std::hash<std::string>()(r.value));
    h = Mix(h, std::hash<std::string>()(r.flags));
    h = Mix(h, SymbolHash()(r.symbol));
    for (const Rule &m : r.members) h = Mix(h, (*this)(m));
    if (r.params) {
        const MetadataParams &p = *r.params;
        h = Mix(h, static_cast<size_t>(p.precedence.kind) * 31 + static_cast<size_t>(p.precedence.integer));
        h = Mix(h, std::hash<std::string>()(p.precedence.name));
        h = Mix(h, static_cast<size_t>(p.dynamic_precedence));
        h = Mix(h, p.associativity ? 1 + static_cast<size_t>(*p.associativity) : 0);
        h = Mix(h, p.is_token * 2 + p.is_main_token);
        if (p.alias) h = Mix(h, AliasHash()(*p.alias));
        if (p.field_name) h = Mix(h, std::hash<std::string>()(*p.field_name));
    }
    return h;
}

// ---- TokenSet ---------------------------------------------------------------

int CompareBitVec(const BitVec &a, const BitVec &b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] ? 1 : -1;
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

std::strong_ordering TokenSet::operator<=>(const TokenSet &o) const {
    if (int c = CompareBitVec(terminal_bits, o.terminal_bits)) return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    if (int c = CompareBitVec(external_bits, o.external_bits)) return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
    if (eof != o.eof) return eof <=> o.eof;
    return end_of_nonterminal_extra <=> o.end_of_nonterminal_extra;
}

std::vector<Symbol> TokenSet::Symbols() const {
    std::vector<Symbol> result;
    for (size_t i = 0; i < terminal_bits.size(); i++)
        if (terminal_bits[i]) result.push_back(Symbol::Terminal(i));
    for (size_t i = 0; i < external_bits.size(); i++)
        if (external_bits[i]) result.push_back(Symbol::External(i));
    if (eof) result.push_back(Symbol::End());
    if (end_of_nonterminal_extra) result.push_back(Symbol::EndOfNonTerminalExtra());
    return result;
}

std::vector<Symbol> TokenSet::Terminals() const {
    std::vector<Symbol> result;
    for (size_t i = 0; i < terminal_bits.size(); i++)
        if (terminal_bits[i]) result.push_back(Symbol::Terminal(i));
    return result;
}

bool TokenSet::Contains(const Symbol &s) const {
    switch (s.kind) {
        case SymbolType::NonTerminal:
            Fail("Cannot store non-terminals in a TokenSet");
        case SymbolType::Terminal:
            return s.index < terminal_bits.size() && terminal_bits[s.index];
        case SymbolType::External:
            return s.index < external_bits.size() && external_bits[s.index];
        case SymbolType::End:
            return eof;
        case SymbolType::EndOfNonTerminalExtra:
            return end_of_nonterminal_extra;
    }
    return false;
}

void TokenSet::Insert(const Symbol &s) {
    BitVec *vec = nullptr;
    switch (s.kind) {
        case SymbolType::NonTerminal:
            Fail("Cannot store non-terminals in a TokenSet");
        case SymbolType::Terminal:
            vec = &terminal_bits;
            break;
        case SymbolType::External:
            vec = &external_bits;
            break;
        case SymbolType::End:
            eof = true;
            return;
        case SymbolType::EndOfNonTerminalExtra:
            end_of_nonterminal_extra = true;
            return;
    }
    if (s.index >= vec->size()) vec->resize(s.index + 1, false);
    (*vec)[s.index] = true;
}

bool TokenSet::Remove(const Symbol &s) {
    BitVec *vec = nullptr;
    switch (s.kind) {
        case SymbolType::NonTerminal:
            Fail("Cannot store non-terminals in a TokenSet");
        case SymbolType::Terminal:
            vec = &terminal_bits;
            break;
        case SymbolType::External:
            vec = &external_bits;
            break;
        case SymbolType::End: {
            bool had = eof;
            eof = false;
            return had;
        }
        case SymbolType::EndOfNonTerminalExtra: {
            bool had = end_of_nonterminal_extra;
            end_of_nonterminal_extra = false;
            return had;
        }
    }
    if (s.index < vec->size() && (*vec)[s.index]) {
        (*vec)[s.index] = false;
        while (!vec->empty() && !vec->back()) vec->pop_back();
        return true;
    }
    return false;
}

bool TokenSet::IsEmpty() const {
    if (eof || end_of_nonterminal_extra) return false;
    for (bool b : terminal_bits)
        if (b) return false;
    for (bool b : external_bits)
        if (b) return false;
    return true;
}

size_t TokenSet::Len() const {
    size_t n = eof + end_of_nonterminal_extra;
    for (bool b : terminal_bits) n += b;
    for (bool b : external_bits) n += b;
    return n;
}

static bool InsertAllBits(BitVec &self, const BitVec &other) {
    bool result = false;
    if (other.size() > self.size()) self.resize(other.size(), false);
    for (size_t i = 0; i < other.size(); i++) {
        if (other[i]) {
            result |= !self[i];
            self[i] = true;
        }
    }
    return result;
}

bool TokenSet::InsertAllTerminals(const TokenSet &o) { return InsertAllBits(terminal_bits, o.terminal_bits); }
bool TokenSet::InsertAllExternals(const TokenSet &o) { return InsertAllBits(external_bits, o.external_bits); }

bool TokenSet::InsertAll(const TokenSet &o) {
    bool result = false;
    if (o.eof) {
        result |= !eof;
        eof = true;
    }
    if (o.end_of_nonterminal_extra) {
        result |= !end_of_nonterminal_extra;
        end_of_nonterminal_extra = true;
    }
    result |= InsertAllTerminals(o);
    result |= InsertAllExternals(o);
    return result;
}

size_t TokenSet::Hash() const {
    size_t h = terminal_bits.size() * 1315423911u + external_bits.size();
    size_t word = 0;
    for (size_t i = 0; i < terminal_bits.size(); i++) {
        word = word * 2 + terminal_bits[i];
        if (i % 60 == 59) h = Mix(h, word), word = 0;
    }
    h = Mix(h, word);
    word = 0;
    for (bool b : external_bits) word = word * 2 + b;
    h = Mix(h, word);
    return Mix(h, eof * 2 + end_of_nonterminal_extra);
}

// ---- CharacterSet -----------------------------------------------------------

CharacterSet CharacterSet::Negate() const {
    CharacterSet self = *this;
    size_t i = 0;
    uint32_t previous_end = 0;
    while (i < self.ranges.size()) {
        auto range = self.ranges[i];
        uint32_t start = previous_end;
        previous_end = range.second;
        if (start < range.first) {
            self.ranges[i] = {start, range.first};
            i++;
        } else {
            self.ranges.erase(self.ranges.begin() + static_cast<long>(i));
        }
    }
    if (previous_end < kCharEnd) self.ranges.push_back({previous_end, kCharEnd});
    return self;
}

CharacterSet CharacterSet::AddChar(uint32_t c) const {
    CharacterSet self = *this;
    self.AddIntRange(0, c, c + 1);
    return self;
}

CharacterSet CharacterSet::AddRange(uint32_t start, uint32_t end_inclusive) const {
    CharacterSet self = *this;
    self.AddIntRange(0, start, end_inclusive + 1);
    return self;
}

CharacterSet CharacterSet::Add(const CharacterSet &o) const {
    CharacterSet self = *this;
    size_t index = 0;
    for (const auto &r : o.ranges) index = self.AddIntRange(index, r.first, r.second);
    return self;
}

size_t CharacterSet::AddIntRange(size_t i, uint32_t start, uint32_t end) {
    while (i < ranges.size()) {
        auto &range = ranges[i];
        if (range.first > end) {
            ranges.insert(ranges.begin() + static_cast<long>(i), {start, end});
            return i;
        }
        if (range.second >= start) {
            range.second = std::max(range.second, end);
            range.first = std::min(range.first, start);
            while (i + 1 < ranges.size() && ranges[i + 1].first <= ranges[i].second) {
                ranges[i].second = std::max(ranges[i].second, ranges[i + 1].second);
                ranges.erase(ranges.begin() + static_cast<long>(i) + 1);
            }
            return i;
        }
        i++;
    }
    ranges.push_back({start, end});
    return i;
}

bool CharacterSet::DoesIntersect(const CharacterSet &o) const {
    size_t l = 0, r = 0;
    while (l < ranges.size() && r < o.ranges.size()) {
        const auto &left = ranges[l];
        const auto &right = o.ranges[r];
        if (left.second <= right.first) {
            l++;
        } else if (left.first >= right.second) {
            r++;
        } else {
            return true;
        }
    }
    return false;
}

CharacterSet CharacterSet::RemoveIntersection(CharacterSet &other) {
    std::vector<std::pair<uint32_t, uint32_t>> intersection;
    size_t left_i = 0, right_i = 0;
    auto &L = ranges;
    auto &R = other.ranges;
    while (left_i < L.size() && right_i < R.size()) {
        auto &left = L[left_i];
        auto &right = R[right_i];
        if (left.first < right.first) {
            if (left.second <= right.first) {
                left_i++;
                continue;
            }
            if (left.second < right.second) {
                intersection.push_back({right.first, left.second});
                std::swap(left.second, right.first);
                left_i++;
            } else if (left.second == right.second) {
                intersection.push_back(right);
                left.second = right.first;
                R.erase(R.begin() + static_cast<long>(right_i));
            } else {
                intersection.push_back(right);
                std::pair<uint32_t, uint32_t> new_range{left.first, right.first};
                left.first = right.second;
                L.insert(L.begin() + static_cast<long>(left_i), new_range);
                R.erase(R.begin() + static_cast<long>(right_i));
                left_i++;
            }
        } else if (left.first == right.first) {
            if (left.second < right.second) {
                intersection.push_back(left);
                right.first = left.second;
                L.erase(L.begin() + static_cast<long>(left_i));
            } else if (left.second == right.second) {
                intersection.push_back(left);
                L.erase(L.begin() + static_cast<long>(left_i));
                R.erase(R.begin() + static_cast<long>(right_i));
            } else {
                intersection.push_back(right);
                left.first = right.second;
                R.erase(R.begin() + static_cast<long>(right_i));
            }
        } else {
            if (left.first >= right.second) {
                right_i++;
                continue;
            }
            if (left.second < right.second) {
                intersection.push_back(left);
                std::pair<uint32_t, uint32_t> new_range{right.first, left.first};
                right.first = left.second;
                R.insert(R.begin() + static_cast<long>(right_i), new_range);
                L.erase(L.begin() + static_cast<long>(left_i));
                right_i++;
            } else if (left.second == right.second) {
                intersection.push_back(left);
                right.second = left.first;
                L.erase(L.begin() + static_cast<long>(left_i));
            } else {
                intersection.push_back({left.first, right.second});
                std::swap(left.first, right.second);
                right_i++;
            }
        }
    }
    return CharacterSet{std::move(intersection)};
}

CharacterSet CharacterSet::Difference(CharacterSet o) const {
    CharacterSet self = *this;
    self.RemoveIntersection(o);
    return self;
}

static bool IsScalar(uint32_t c) { return c < 0xD800 || (c > 0xDFFF && c < kCharEnd); }

std::vector<std::pair<uint32_t, uint32_t>> CharacterSet::CharRanges() const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    for (const auto &r : ranges) {
        uint32_t start = r.first;
        while (start < r.second && !IsScalar(start)) start++;
        if (start >= r.second) continue;
        uint32_t end = r.second - 1;
        while (end > start && !IsScalar(end)) end--;
        result.push_back({start, end});
    }
    return result;
}

CharacterSet CharacterSet::SimplifyIgnoring(const CharacterSet &ruled_out) const {
    CharacterSet result;
    std::optional<std::pair<uint32_t, uint32_t>> prev;
    for (size_t i = 0; i <= ranges.size(); i++) {
        if (i < ranges.size()) {
            const auto &range = ranges[i];
            if (ruled_out.ContainsCodepointRange(range.first, range.second)) continue;
            if (prev && ruled_out.ContainsCodepointRange(prev->second, range.first)) {
                prev->second = range.second;
                continue;
            }
            if (prev) result.ranges.push_back(*prev);
            prev = range;
        } else if (prev) {
            result.ranges.push_back(*prev);
        }
    }
    return result;
}

bool CharacterSet::ContainsCodepointRange(uint32_t start, uint32_t end) const {
    // binary_search_by: Less while probe.end <= start, Greater when probe.start > start.
    size_t lo = 0, hi = ranges.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ranges[mid].second <= start) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= ranges.size()) return false;
    return ranges[lo].first <= start && ranges[lo].second >= end;
}

std::strong_ordering CharacterSet::operator<=>(const CharacterSet &o) const {
    uint64_t a = 0, b = 0;
    for (const auto &r : ranges) a += r.second - r.first;
    for (const auto &r : o.ranges) b += r.second - r.first;
    if (a != b) return a <=> b;
    size_t n = std::min(ranges.size(), o.ranges.size());
    for (size_t i = 0; i < n; i++) {
        uint32_t la = ranges[i].second - ranges[i].first, lb = o.ranges[i].second - o.ranges[i].first;
        if (la != lb) return la <=> lb;
        if (la > 0 && ranges[i].first != o.ranges[i].first) return ranges[i].first <=> o.ranges[i].first;
    }
    return std::strong_ordering::equal;
}

size_t CharacterSet::Hash() const {
    size_t h = ranges.size();
    for (const auto &r : ranges) h = Mix(h, (static_cast<size_t>(r.first) << 21) ^ r.second);
    return h;
}

// ---- NfaCursor --------------------------------------------------------------

void NfaCursor::Reset(std::vector<uint32_t> states) {
    state_ids.clear();
    AddStates(states);
}

std::vector<std::pair<const CharacterSet *, bool>> NfaCursor::TransitionChars() const {
    std::vector<std::pair<const CharacterSet *, bool>> result;
    for (uint32_t id : state_ids) {
        const NfaState &s = nfa_->states[id];
        if (s.kind == NfaState::Kind::Advance) result.push_back({&s.chars, s.is_sep});
    }
    return result;
}

std::vector<NfaTransition> NfaCursor::Transitions() const {
    std::vector<NfaTransition> result;
    for (uint32_t id : state_ids) {
        const NfaState &s = nfa_->states[id];
        if (s.kind != NfaState::Kind::Advance) continue;
        CharacterSet chars = s.chars;
        bool is_sep = s.is_sep;
        int32_t prec = s.precedence;
        uint32_t state = s.state_id;
        size_t i = 0;
        while (i < result.size() && !chars.IsEmpty()) {
            CharacterSet intersection = result[i].characters.RemoveIntersection(chars);
            if (!intersection.IsEmpty()) {
                std::vector<uint32_t> intersection_states = result[i].states;
                auto pos = std::lower_bound(intersection_states.begin(), intersection_states.end(), state);
                if (pos == intersection_states.end() || *pos != state) intersection_states.insert(pos, state);
                NfaTransition t;
                t.characters = std::move(intersection);
                t.is_separator = result[i].is_separator && is_sep;
                t.precedence = std::max(result[i].precedence, prec);
                t.states = std::move(intersection_states);
                if (result[i].characters.IsEmpty()) {
                    result[i] = std::move(t);
                } else {
                    result.insert(result.begin() + static_cast<long>(i), std::move(t));
                    i++;
                }
            }
            i++;
        }
        if (!chars.IsEmpty()) {
            NfaTransition t;
            t.characters = std::move(chars);
            t.precedence = prec;
            t.states = {state};
            t.is_separator = is_sep;
            result.push_back(std::move(t));
        }
    }
    size_t i = 0;
    while (i < result.size()) {
        for (size_t j = 0; j < i; j++) {
            if (result[j].states == result[i].states && result[j].is_separator == result[i].is_separator &&
                result[j].precedence == result[i].precedence) {
                result[j].characters = result[j].characters.Add(result[i].characters);
                result.erase(result.begin() + static_cast<long>(i));
                i--;
                break;
            }
        }
        i++;
    }
    // sort_unstable_by characters: the sets are disjoint, so no two compare equal.
    std::sort(result.begin(), result.end(),
              [](const NfaTransition &a, const NfaTransition &b) { return (a.characters <=> b.characters) < 0; });
    return result;
}

std::vector<std::pair<size_t, int32_t>> NfaCursor::Completions() const {
    std::vector<std::pair<size_t, int32_t>> result;
    for (uint32_t id : state_ids) {
        const NfaState &s = nfa_->states[id];
        if (s.kind == NfaState::Kind::Accept) result.push_back({s.variable_index, s.precedence});
    }
    return result;
}

void NfaCursor::AddStates(std::vector<uint32_t> &new_state_ids) {
    size_t i = 0;
    while (i < new_state_ids.size()) {
        uint32_t state_id = new_state_ids[i];
        const NfaState &state = nfa_->states[state_id];
        if (state.kind == NfaState::Kind::Split) {
            uint32_t left = state.state_id, right = state.right;
            bool has_left = false, has_right = false;
            for (uint32_t id : new_state_ids) {
                if (id == left) has_left = true;
                if (id == right) has_right = true;
            }
            if (!has_left) new_state_ids.push_back(left);
            if (!has_right) new_state_ids.push_back(right);
        } else {
            auto pos = std::lower_bound(state_ids.begin(), state_ids.end(), state_id);
            if (pos == state_ids.end() || *pos != state_id) state_ids.insert(pos, state_id);
        }
        i++;
    }
}

// ---- grammars ---------------------------------------------------------------

size_t LexicalGrammar::VariableIndexForNfaState(uint32_t state_id) const {
    for (size_t i = 0; i < variables.size(); i++)
        if (variables[i].start_state >= state_id) return i;
    Fail("NFA state outside every lexical variable");
}

std::vector<size_t> LexicalGrammar::VariableIndicesForNfaStates(const std::vector<uint32_t> &state_ids) const {
    std::vector<size_t> result;
    for (uint32_t id : state_ids) {
        size_t v = VariableIndexForNfaState(id);
        if (result.empty() || result.back() != v) result.push_back(v);
    }
    return result;
}

template <class T>
static std::strong_ordering CompareOptional(const std::optional<T> &a, const std::optional<T> &b) {
    if (a.has_value() != b.has_value()) return a.has_value() <=> b.has_value();
    if (!a) return std::strong_ordering::equal;
    return *a <=> *b;
}

std::strong_ordering ProductionStep::operator<=>(const ProductionStep &o) const {
    if (auto c = symbol <=> o.symbol; c != 0) return c;
    if (auto c = precedence <=> o.precedence; c != 0) return c;
    if (auto c = CompareOptional(associativity, o.associativity); c != 0) return c;
    if (auto c = CompareOptional(alias, o.alias); c != 0) return c;
    if (auto c = CompareOptional(field_name, o.field_name); c != 0) return c;
    return reserved_word_set_id <=> o.reserved_word_set_id;
}

}  // namespace tsgen
