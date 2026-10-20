// InputGrammar -> syntax + lexical grammars: a port of prepare_grammar/*.rs
// (intern_symbols, extract_tokens, expand_repeats, flatten_grammar,
// expand_tokens, extract_default_aliases, process_inlines and the
// validations in mod.rs).
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "generate.h"
#include "regex.h"

namespace tsgen {

namespace {

// ---- mod.rs: validations -------------------------------------------------------

void ValidatePrecedences(const InputGrammar &grammar) {
    std::map<std::pair<PrecedenceEntry, PrecedenceEntry>, int> pairs;
    for (const auto &list : grammar.precedence_orderings) {
        for (size_t i = 0; i < list.size(); i++) {
            for (size_t j = i + 1; j < list.size(); j++) {
                const PrecedenceEntry *e1 = &list[i], *e2 = &list[j];
                if (*e1 == *e2) continue;
                int ordering = 1;
                if (*e1 > *e2) {
                    ordering = -1;
                    std::swap(e1, e2);
                }
                auto [it, inserted] = pairs.emplace(std::make_pair(*e1, *e2), ordering);
                if (!inserted && it->second != ordering)
                    Fail("Conflicting orderings for precedences " + e1->ToString() + " and " + e2->ToString());
            }
        }
    }
    std::set<std::string> names;
    for (const auto &list : grammar.precedence_orderings)
        for (const PrecedenceEntry &e : list)
            if (e.kind == PrecedenceEntry::Kind::Name) names.insert(e.value);
    std::function<void(const std::string &, const Rule &)> validate = [&](const std::string &rule_name, const Rule &rule) {
        switch (rule.kind) {
            case Rule::Kind::Repeat:
                validate(rule_name, rule.Inner());
                break;
            case Rule::Kind::Seq:
            case Rule::Kind::Choice:
                for (const Rule &e : rule.members) validate(rule_name, e);
                break;
            case Rule::Kind::Metadata:
                if (rule.Params().precedence.kind == Precedence::Kind::Name && !names.count(rule.Params().precedence.name))
                    Fail("Undeclared precedence '" + rule.Params().precedence.name + "' in rule '" + rule_name + "'");
                validate(rule_name, rule.Inner());
                break;
            default:
                break;
        }
    };
    for (const Variable &v : grammar.variables) validate(v.name, v.rule);
}

std::set<std::string> GetSingleSymbolProductions(const Rule &rule) {
    switch (rule.kind) {
        case Rule::Kind::NamedSymbol:
            return {rule.value};
        case Rule::Kind::Choice: {
            std::set<std::string> result;
            for (const Rule &c : rule.members) {
                auto s = GetSingleSymbolProductions(c);
                result.insert(s.begin(), s.end());
            }
            return result;
        }
        case Rule::Kind::Metadata:
            return GetSingleSymbolProductions(rule.Inner());
        default:
            return {};
    }
}

void ValidateIndirectRecursion(const InputGrammar &grammar) {
    std::vector<std::pair<std::string, std::set<std::string>>> transitions;  // an IndexMap
    for (const Variable &v : grammar.variables) {
        std::set<std::string> filtered;
        for (const std::string &s : GetSingleSymbolProductions(v.rule))
            if (s != v.name) filtered.insert(s);
        bool found = false;
        for (auto &t : transitions)
            if (t.first == v.name) t.second = filtered, found = true;
        if (!found) transitions.push_back({v.name, filtered});
    }
    auto get = [&](const std::string &name) -> const std::set<std::string> * {
        for (const auto &t : transitions)
            if (t.first == name) return &t.second;
        return nullptr;
    };
    std::function<std::optional<std::pair<size_t, size_t>>(const std::string &, std::set<std::string> &,
                                                           std::vector<std::string> &)>
        get_cycle = [&](const std::string &current, std::set<std::string> &visited,
                        std::vector<std::string> &path) -> std::optional<std::pair<size_t, size_t>> {
        for (size_t i = 0; i < path.size(); i++) {
            if (path[i] == current) {
                path.push_back(current);
                return std::make_pair(i, path.size() - 1);
            }
        }
        if (visited.count(current)) return std::nullopt;
        path.push_back(current);
        visited.insert(current);
        if (const auto *next = get(current))
            for (const std::string &n : *next)
                if (auto c = get_cycle(n, visited, path)) return c;
        path.pop_back();
        return std::nullopt;
    };
    for (const auto &t : transitions) {
        std::set<std::string> visited;
        std::vector<std::string> path;
        if (auto c = get_cycle(t.first, visited, path)) {
            std::string message = "Grammar contains an indirectly recursive rule: ";
            for (size_t i = c->first; i <= c->second; i++) {
                if (i > c->first) message += " -> ";
                message += path[i];
            }
            Fail(message);
        }
    }
}

// ---- intern_symbols.rs -----------------------------------------------------------

VariableType VariableTypeForName(const std::string &name) {
    return !name.empty() && name[0] == '_' ? VariableType::Hidden : VariableType::Named;
}

struct InternedGrammar {
    std::vector<Variable> variables;
    std::vector<Rule> extra_symbols;
    std::vector<std::vector<Symbol>> expected_conflicts;
    std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
    std::vector<Variable> external_tokens;
    std::vector<Symbol> variables_to_inline;
    std::vector<Symbol> supertype_symbols;
    std::optional<Symbol> word_token;
    std::vector<ReservedWordContext<Rule>> reserved_word_sets;
};

struct Interner {
    const InputGrammar &grammar;

    std::optional<Symbol> InternName(const std::string &symbol) const {
        for (size_t i = 0; i < grammar.variables.size(); i++)
            if (grammar.variables[i].name == symbol) return Symbol::NonTerminal(i);
        for (size_t i = 0; i < grammar.external_tokens.size(); i++)
            if (grammar.external_tokens[i].kind == Rule::Kind::NamedSymbol && grammar.external_tokens[i].value == symbol)
                return Symbol::External(i);
        return std::nullopt;
    }

    void CheckSingle(const std::vector<Rule> &elements, const std::string *name) const {
        if (elements.size() == 1 && (elements[0].kind == Rule::Kind::String || elements[0].kind == Rule::Kind::Pattern))
            std::fprintf(stderr,
                         "Warning: rule %s contains a `seq` or `choice` rule with a single element. This is unnecessary.\n",
                         name ? name->c_str() : "");
    }

    Rule InternRule(const Rule &rule, const std::string *name) const {
        switch (rule.kind) {
            case Rule::Kind::Choice:
            case Rule::Kind::Seq: {
                CheckSingle(rule.members, name);
                Rule r = rule;
                for (Rule &m : r.members) m = InternRule(m, name);
                return r;
            }
            case Rule::Kind::Repeat:
            case Rule::Kind::Metadata:
            case Rule::Kind::Reserved: {
                Rule r = rule;
                r.members[0] = InternRule(rule.Inner(), name);
                return r;
            }
            case Rule::Kind::NamedSymbol: {
                auto symbol = InternName(rule.value);
                if (!symbol) Fail("Undefined symbol `" + rule.value + "`");
                return Rule::Sym(*symbol);
            }
            default:
                return rule;
        }
    }
};

InternedGrammar InternSymbols(const InputGrammar &grammar) {
    Interner interner{grammar};
    if (VariableTypeForName(grammar.variables[0].name) == VariableType::Hidden)
        Fail("A grammar's start rule must be visible.");
    InternedGrammar out;
    for (const Variable &v : grammar.variables)
        out.variables.push_back({v.name, VariableTypeForName(v.name), interner.InternRule(v.rule, &v.name)});
    for (const Rule &external : grammar.external_tokens) {
        Rule rule = interner.InternRule(external, nullptr);
        std::string name;
        VariableType kind = VariableType::Anonymous;
        if (external.kind == Rule::Kind::NamedSymbol) {
            name = external.value;
            kind = VariableTypeForName(name);
        }
        out.external_tokens.push_back({name, kind, rule});
    }
    for (const Rule &extra : grammar.extra_symbols) out.extra_symbols.push_back(interner.InternRule(extra, nullptr));
    for (const std::string &s : grammar.supertype_symbols) {
        auto symbol = interner.InternName(s);
        if (!symbol) Fail("Undefined symbol `" + s + "` in grammar's supertypes array");
        out.supertype_symbols.push_back(*symbol);
    }
    for (const auto &set : grammar.reserved_words) {
        ReservedWordContext<Rule> interned{set.name, {}};
        for (const Rule &r : set.reserved_words) interned.reserved_words.push_back(interner.InternRule(r, nullptr));
        out.reserved_word_sets.push_back(std::move(interned));
    }
    for (const auto &conflict : grammar.expected_conflicts) {
        std::vector<Symbol> interned;
        for (const std::string &name : conflict) {
            auto symbol = interner.InternName(name);
            if (!symbol) Fail("Undefined symbol `" + name + "` in grammar's conflicts array");
            interned.push_back(*symbol);
        }
        out.expected_conflicts.push_back(std::move(interned));
    }
    for (const std::string &name : grammar.variables_to_inline)
        if (auto symbol = interner.InternName(name)) out.variables_to_inline.push_back(*symbol);
    if (grammar.word_token) {
        auto symbol = interner.InternName(*grammar.word_token);
        if (!symbol) Fail("Undefined symbol `" + *grammar.word_token + "` as grammar's word token");
        out.word_token = symbol;
    }
    for (size_t i = 0; i < out.variables.size(); i++)
        if (Contains(out.supertype_symbols, Symbol::NonTerminal(i))) out.variables[i].kind = VariableType::Hidden;
    out.precedence_orderings = grammar.precedence_orderings;
    return out;
}

// ---- extract_tokens.rs -------------------------------------------------------------

struct ExtractedSyntaxGrammar {
    std::vector<Variable> variables;
    std::vector<Symbol> extra_symbols;
    std::vector<std::vector<Symbol>> expected_conflicts;
    std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
    std::vector<ExternalToken> external_tokens;
    std::vector<Symbol> variables_to_inline;
    std::vector<Symbol> supertype_symbols;
    std::optional<Symbol> word_token;
    std::vector<ReservedWordContext<Symbol>> reserved_word_sets;
};

struct ExtractedLexicalGrammar {
    std::vector<Variable> variables;
    std::vector<Rule> separators;
};

struct TokenExtractor {
    std::string current_variable_name;
    size_t current_variable_token_count = 0;
    bool is_first_rule = false;
    std::vector<Variable> extracted_variables;
    std::vector<size_t> extracted_usage_counts;

    void ExtractTokensInVariable(bool is_first, Variable &variable) {
        current_variable_name = variable.name;
        current_variable_token_count = 0;
        is_first_rule = is_first;
        variable.rule = ExtractTokensInRule(variable.rule);
    }

    Rule ExtractTokensInRule(const Rule &input) {
        switch (input.kind) {
            case Rule::Kind::String:
                return Rule::Sym(ExtractToken(input, &input.value));
            case Rule::Kind::Pattern:
                return Rule::Sym(ExtractToken(input, nullptr));
            case Rule::Kind::Metadata: {
                if (input.Params().is_token) {
                    MetadataParams params = input.Params();
                    params.is_token = false;
                    const std::string *string_value = nullptr;
                    if (input.Inner().kind == Rule::Kind::String) string_value = &input.Inner().value;
                    const Rule &rule_to_extract = params == MetadataParams{} ? input.Inner() : input;
                    return Rule::Sym(ExtractToken(rule_to_extract, string_value));
                }
                Rule r = input;
                r.members[0] = ExtractTokensInRule(input.Inner());
                return r;
            }
            case Rule::Kind::Repeat:
            case Rule::Kind::Reserved: {
                Rule r = input;
                r.members[0] = ExtractTokensInRule(input.Inner());
                return r;
            }
            case Rule::Kind::Seq:
            case Rule::Kind::Choice: {
                Rule r = input;
                for (Rule &m : r.members) m = ExtractTokensInRule(m);
                return r;
            }
            default:
                return input;
        }
    }

    Symbol ExtractToken(const Rule &rule, const std::string *string_value) {
        for (size_t i = 0; i < extracted_variables.size(); i++) {
            if (extracted_variables[i].rule == rule) {
                extracted_usage_counts[i]++;
                return Symbol::Terminal(i);
            }
        }
        size_t index = extracted_variables.size();
        Variable variable;
        if (string_value) {
            if (string_value->empty() && !is_first_rule)
                Fail("The rule `" + current_variable_name +
                     "` contains an empty string.\nTree-sitter does not support syntactic rules that contain an empty "
                     "string\nunless they are used only as the grammar's start rule.\n");
            variable = {*string_value, VariableType::Anonymous, rule};
        } else {
            current_variable_token_count++;
            variable = {current_variable_name + "_token" + std::to_string(current_variable_token_count),
                        VariableType::Auxiliary, rule};
        }
        extracted_variables.push_back(std::move(variable));
        extracted_usage_counts.push_back(1);
        return Symbol::Terminal(index);
    }
};

struct SymbolReplacer {
    std::map<size_t, size_t> replacements;

    Symbol ReplaceSymbol(Symbol symbol) const {
        if (!symbol.IsNonTerminal()) return symbol;
        auto it = replacements.find(symbol.index);
        if (it != replacements.end()) return Symbol::Terminal(it->second);
        size_t adjusted = symbol.index;
        for (const auto &kv : replacements)
            if (kv.first < symbol.index) adjusted--;
        return Symbol::NonTerminal(adjusted);
    }

    Rule ReplaceSymbolsInRule(const Rule &rule) const {
        switch (rule.kind) {
            case Rule::Kind::Symbol:
                return Rule::Sym(ReplaceSymbol(rule.symbol));
            case Rule::Kind::Choice:
            case Rule::Kind::Seq:
            case Rule::Kind::Repeat:
            case Rule::Kind::Metadata:
            case Rule::Kind::Reserved: {
                Rule r = rule;
                for (Rule &m : r.members) m = ReplaceSymbolsInRule(m);
                return r;
            }
            default:
                return rule;
        }
    }
};

std::pair<ExtractedSyntaxGrammar, ExtractedLexicalGrammar> ExtractTokens(InternedGrammar grammar) {
    TokenExtractor extractor;
    for (size_t i = 0; i < grammar.variables.size(); i++) extractor.ExtractTokensInVariable(i == 0, grammar.variables[i]);
    for (Variable &v : grammar.external_tokens) extractor.ExtractTokensInVariable(false, v);

    std::vector<Variable> lexical_variables = std::move(extractor.extracted_variables);
    std::vector<Variable> variables;
    SymbolReplacer replacer;
    for (size_t i = 0; i < grammar.variables.size(); i++) {
        Variable &variable = grammar.variables[i];
        if (variable.rule.kind == Rule::Kind::Symbol && variable.rule.symbol.kind == SymbolType::Terminal) {
            size_t index = variable.rule.symbol.index;
            if (i > 0 && extractor.extracted_usage_counts[index] == 1) {
                Variable &lexical_variable = lexical_variables[index];
                if (lexical_variable.kind == VariableType::Auxiliary || variable.kind != VariableType::Hidden) {
                    lexical_variable.kind = variable.kind;
                    lexical_variable.name = variable.name;
                    replacer.replacements[i] = index;
                    continue;
                }
            }
        }
        variables.push_back(std::move(variable));
    }
    for (Variable &v : variables) v.rule = replacer.ReplaceSymbolsInRule(v.rule);

    ExtractedSyntaxGrammar syntax;
    for (const auto &conflict : grammar.expected_conflicts) {
        std::vector<Symbol> result;
        for (const Symbol &s : conflict) result.push_back(replacer.ReplaceSymbol(s));
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        syntax.expected_conflicts.push_back(std::move(result));
    }
    for (const Symbol &s : grammar.supertype_symbols) syntax.supertype_symbols.push_back(replacer.ReplaceSymbol(s));
    for (const Symbol &s : grammar.variables_to_inline) syntax.variables_to_inline.push_back(replacer.ReplaceSymbol(s));

    std::vector<Rule> separators;
    for (const Rule &rule : grammar.extra_symbols) {
        if (rule.kind == Rule::Kind::Symbol) {
            syntax.extra_symbols.push_back(replacer.ReplaceSymbol(rule.symbol));
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < lexical_variables.size(); i++) {
            if (lexical_variables[i].rule == rule) {
                syntax.extra_symbols.push_back(Symbol::Terminal(i));
                found = true;
                break;
            }
        }
        if (!found) separators.push_back(rule);
    }

    for (const Variable &external : grammar.external_tokens) {
        Rule rule = replacer.ReplaceSymbolsInRule(external.rule);
        if (rule.kind != Rule::Kind::Symbol) Fail("Non-symbol rules cannot be used as external tokens");
        Symbol symbol = rule.symbol;
        if (symbol.IsNonTerminal())
            Fail("Rule '" + variables[symbol.index].name + "' cannot be used as both an external token and a non-terminal rule");
        if (symbol.IsExternal()) {
            syntax.external_tokens.push_back({external.name, external.kind, std::nullopt});
        } else {
            syntax.external_tokens.push_back({lexical_variables[symbol.index].name, external.kind, symbol});
        }
    }

    if (grammar.word_token) {
        Symbol token = replacer.ReplaceSymbol(*grammar.word_token);
        if (token.IsNonTerminal()) {
            const Variable &word = variables[token.index];
            std::string message = "Non-terminal symbol '" + word.name + "' cannot be used as the word token";
            for (size_t i = 0; i < variables.size(); i++) {
                if (i != token.index && variables[i].rule == word.rule) {
                    message += ", because its rule is duplicated in '" + variables[i].name + "'";
                    break;
                }
            }
            Fail(message);
        }
        syntax.word_token = token;
    }

    for (const auto &context : grammar.reserved_word_sets) {
        ReservedWordContext<Symbol> out{context.name, {}};
        for (const Rule &reserved : context.reserved_words) {
            if (reserved.kind == Rule::Kind::Symbol) {
                out.reserved_words.push_back(replacer.ReplaceSymbol(reserved.symbol));
                continue;
            }
            bool found = false;
            for (size_t i = 0; i < lexical_variables.size(); i++) {
                if (lexical_variables[i].rule == reserved) {
                    out.reserved_words.push_back(Symbol::Terminal(i));
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::string token_name = reserved.kind == Rule::Kind::String || reserved.kind == Rule::Kind::Pattern
                                             ? reserved.value
                                             : "unknown";
                Fail("Reserved word '" + token_name + "' must be a token");
            }
        }
        syntax.reserved_word_sets.push_back(std::move(out));
    }

    syntax.variables = std::move(variables);
    syntax.precedence_orderings = grammar.precedence_orderings;
    return {std::move(syntax), ExtractedLexicalGrammar{std::move(lexical_variables), std::move(separators)}};
}

// ---- expand_repeats.rs ---------------------------------------------------------------

struct Expander {
    std::string variable_name;
    size_t repeat_count_in_variable = 0;
    size_t preceding_symbol_count = 0;
    std::vector<Variable> auxiliary_variables;
    std::unordered_map<Rule, Symbol, RuleHash> existing_repeats;

    static Rule WrapRuleInBinaryTree(Symbol symbol, Rule rule) {
        std::vector<Rule> choices;
        choices.push_back(Rule::SeqOf({Rule::Sym(symbol), Rule::Sym(symbol)}));
        choices.push_back(std::move(rule));
        return Rule::ChoiceOf(std::move(choices));
    }

    bool ExpandVariable(size_t index, Variable &variable) {
        variable_name = variable.name;
        repeat_count_in_variable = 0;
        Rule rule = std::move(variable.rule);
        variable.rule = Rule::Blank();
        if (variable.kind == VariableType::Hidden && rule.kind == Rule::Kind::Repeat) {
            Rule inner = ExpandRule(rule.Inner());
            variable.rule = WrapRuleInBinaryTree(Symbol::NonTerminal(index), std::move(inner));
            variable.kind = VariableType::Auxiliary;
            return true;
        }
        variable.rule = ExpandRule(rule);
        return false;
    }

    Rule ExpandRule(const Rule &rule) {
        switch (rule.kind) {
            case Rule::Kind::Choice:
            case Rule::Kind::Seq: {
                Rule r = rule;
                for (Rule &m : r.members) m = ExpandRule(m);
                return r;
            }
            case Rule::Kind::Metadata: {
                Rule r = rule;
                r.members[0] = ExpandRule(rule.Inner());
                return r;
            }
            case Rule::Kind::Repeat: {
                Rule inner = ExpandRule(rule.Inner());
                auto it = existing_repeats.find(inner);
                if (it != existing_repeats.end()) return Rule::Sym(it->second);
                repeat_count_in_variable++;
                std::string rule_name = variable_name + "_repeat" + std::to_string(repeat_count_in_variable);
                Symbol repeat_symbol = Symbol::NonTerminal(preceding_symbol_count + auxiliary_variables.size());
                existing_repeats.emplace(inner, repeat_symbol);
                auxiliary_variables.push_back(
                    {rule_name, VariableType::Auxiliary, WrapRuleInBinaryTree(repeat_symbol, std::move(inner))});
                return Rule::Sym(repeat_symbol);
            }
            default:
                return rule;
        }
    }
};

void ExpandRepeats(ExtractedSyntaxGrammar &grammar) {
    Expander expander;
    expander.preceding_symbol_count = grammar.variables.size();
    for (size_t i = 0; i < grammar.variables.size(); i++) {
        if (expander.ExpandVariable(i, grammar.variables[i])) {
            auto &inl = grammar.variables_to_inline;
            inl.erase(std::remove(inl.begin(), inl.end(), Symbol::NonTerminal(i)), inl.end());
        }
    }
    for (Variable &v : expander.auxiliary_variables) grammar.variables.push_back(std::move(v));
}

// ---- flatten_grammar.rs ------------------------------------------------------------------

struct RuleFlattener {
    Production production;
    std::map<std::string, size_t> reserved_word_set_ids;
    std::vector<Precedence> precedence_stack;
    std::vector<Associativity> associativity_stack;
    std::vector<size_t> reserved_word_stack;
    std::vector<Alias> alias_stack;
    std::vector<std::string> field_name_stack;

    SyntaxVariable FlattenVariable(const Variable &variable) {
        std::vector<Rule> choices = ExtractChoices(variable.rule);
        SyntaxVariable out{variable.name, variable.kind, {}};
        for (const Rule &rule : choices) {
            Production p = FlattenRule(rule);
            if (std::find(out.productions.begin(), out.productions.end(), p) == out.productions.end())
                out.productions.push_back(std::move(p));
        }
        return out;
    }

    Production FlattenRule(const Rule &rule) {
        production = Production{};
        alias_stack.clear();
        reserved_word_stack.clear();
        precedence_stack.clear();
        associativity_stack.clear();
        field_name_stack.clear();
        Apply(rule, true);
        return production;
    }

    bool Apply(const Rule &rule, bool at_end) {
        switch (rule.kind) {
            case Rule::Kind::Seq: {
                bool result = false;
                size_t last_index = rule.members.size() - 1;
                for (size_t i = 0; i < rule.members.size(); i++) result |= Apply(rule.members[i], i == last_index && at_end);
                return result;
            }
            case Rule::Kind::Metadata: {
                const MetadataParams &params = rule.Params();
                bool has_precedence = false;
                if (!params.precedence.IsNone()) {
                    has_precedence = true;
                    precedence_stack.push_back(params.precedence);
                }
                bool has_associativity = false;
                if (params.associativity) {
                    has_associativity = true;
                    associativity_stack.push_back(*params.associativity);
                }
                bool has_alias = false;
                if (params.alias) {
                    has_alias = true;
                    alias_stack.push_back(*params.alias);
                }
                bool has_field_name = false;
                if (params.field_name) {
                    has_field_name = true;
                    field_name_stack.push_back(*params.field_name);
                }
                if (std::abs(params.dynamic_precedence) > std::abs(production.dynamic_precedence))
                    production.dynamic_precedence = params.dynamic_precedence;
                bool did_push = Apply(rule.Inner(), at_end);
                if (has_precedence) {
                    precedence_stack.pop_back();
                    if (did_push && !at_end)
                        production.steps.back().precedence = precedence_stack.empty() ? Precedence{} : precedence_stack.back();
                }
                if (has_associativity) {
                    associativity_stack.pop_back();
                    if (did_push && !at_end)
                        production.steps.back().associativity =
                            associativity_stack.empty() ? std::nullopt : std::optional<Associativity>(associativity_stack.back());
                }
                if (has_alias) alias_stack.pop_back();
                if (has_field_name) field_name_stack.pop_back();
                return did_push;
            }
            case Rule::Kind::Reserved: {
                auto it = reserved_word_set_ids.find(rule.value);
                if (it == reserved_word_set_ids.end()) Fail("No such reserved word set: " + rule.value);
                reserved_word_stack.push_back(it->second);
                bool did_push = Apply(rule.Inner(), at_end);
                reserved_word_stack.pop_back();
                return did_push;
            }
            case Rule::Kind::Symbol: {
                ProductionStep step;
                step.symbol = rule.symbol;
                step.precedence = precedence_stack.empty() ? Precedence{} : precedence_stack.back();
                if (!associativity_stack.empty()) step.associativity = associativity_stack.back();
                step.reserved_word_set_id = reserved_word_stack.empty() ? 0 : reserved_word_stack.back();
                if (!alias_stack.empty()) step.alias = alias_stack.back();
                if (!field_name_stack.empty()) step.field_name = field_name_stack.back();
                production.steps.push_back(std::move(step));
                return true;
            }
            default:
                return false;
        }
    }

    static std::vector<Rule> ExtractChoices(const Rule &rule) {
        switch (rule.kind) {
            case Rule::Kind::Seq: {
                std::vector<Rule> result{Rule::Blank()};
                for (const Rule &element : rule.members) {
                    std::vector<Rule> extraction = ExtractChoices(element);
                    std::vector<Rule> next;
                    next.reserve(result.size() * extraction.size());
                    for (const Rule &entry : result)
                        for (const Rule &e : extraction) next.push_back(Rule::SeqOf({entry, e}));
                    result = std::move(next);
                }
                return result;
            }
            case Rule::Kind::Choice: {
                std::vector<Rule> result;
                for (const Rule &element : rule.members)
                    for (Rule &r : ExtractChoices(element)) result.push_back(std::move(r));
                return result;
            }
            case Rule::Kind::Metadata:
            case Rule::Kind::Reserved: {
                std::vector<Rule> result;
                for (Rule &r : ExtractChoices(rule.Inner())) {
                    Rule w = rule;
                    w.members[0] = std::move(r);
                    result.push_back(std::move(w));
                }
                return result;
            }
            default:
                return {rule};
        }
    }
};

SyntaxGrammar FlattenGrammar(ExtractedSyntaxGrammar grammar) {
    std::map<std::string, size_t> ids;
    for (size_t i = 0; i < grammar.reserved_word_sets.size(); i++) ids[grammar.reserved_word_sets[i].name] = i;
    RuleFlattener flattener;
    flattener.reserved_word_set_ids = ids;
    SyntaxGrammar out;
    for (const Variable &v : grammar.variables) out.variables.push_back(flattener.FlattenVariable(v));
    for (size_t i = 0; i < out.variables.size(); i++) {
        Symbol symbol = Symbol::NonTerminal(i);
        bool used = false;
        for (const SyntaxVariable &v : out.variables)
            for (const Production &p : v.productions)
                for (const ProductionStep &s : p.steps)
                    if (s.symbol == symbol) used = true;
        for (const Production &p : out.variables[i].productions) {
            if (used && p.steps.empty())
                Fail("The rule `" + out.variables[i].name +
                     "` matches the empty string.\nTree-sitter does not support syntactic rules that match the empty "
                     "string\nunless they are used only as the grammar's start rule.\n");
            if (Contains(grammar.variables_to_inline, symbol))
                for (const ProductionStep &s : p.steps)
                    if (s.symbol == symbol)
                        Fail("Rule `" + out.variables[i].name + "` cannot be inlined because it contains a reference to itself");
        }
    }
    for (auto &set : grammar.reserved_word_sets) out.reserved_word_sets.push_back(TokenSet::From(set.reserved_words));
    if (out.reserved_word_sets.empty()) out.reserved_word_sets.push_back(TokenSet{});
    out.extra_symbols = std::move(grammar.extra_symbols);
    out.expected_conflicts = std::move(grammar.expected_conflicts);
    out.variables_to_inline = std::move(grammar.variables_to_inline);
    out.precedence_orderings = std::move(grammar.precedence_orderings);
    out.external_tokens = std::move(grammar.external_tokens);
    out.supertype_symbols = std::move(grammar.supertype_symbols);
    out.word_token = grammar.word_token;
    return out;
}

// ---- expand_tokens.rs ----------------------------------------------------------------------

int32_t GetImplicitPrecedence(const Rule &rule) {
    if (rule.kind == Rule::Kind::String) return 2;
    if (rule.kind == Rule::Kind::Metadata)
        return rule.Params().is_main_token ? GetImplicitPrecedence(rule.Inner()) + 1 : GetImplicitPrecedence(rule.Inner());
    return 0;
}

int32_t GetCompletionPrecedence(const Rule &rule) {
    if (rule.kind == Rule::Kind::Metadata && rule.Params().precedence.kind == Precedence::Kind::Integer)
        return rule.Params().precedence.integer;
    return 0;
}

std::string Replace(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::vector<uint32_t> DecodeUtf8(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char b0 = static_cast<unsigned char>(s[i]);
        size_t n = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : 4;
        uint32_t c = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        for (size_t k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(c);
        i += n;
    }
    return out;
}

struct NfaBuilder {
    Nfa nfa;
    bool is_sep = true;
    std::vector<int32_t> precedence_stack{0};
    std::string rule_name;

    bool ExpandRule(const Rule &rule, uint32_t next_state_id) {
        switch (rule.kind) {
            case Rule::Kind::Pattern: {
                std::string s = rule.value;
                s = Replace(s, "\\w", "[0-9A-Za-z_]");
                s = Replace(s, "\\s", "[\\t-\\r ]");
                s = Replace(s, "\\d", "[0-9]");
                s = Replace(s, "\\W", "[^0-9A-Za-z_]");
                s = Replace(s, "\\S", "[^\\t-\\r ]");
                s = Replace(s, "\\D", "[^0-9]");
                regex::Hir hir;
                std::string error;
                if (!regex::Parse(s, rule.flags.find('i') != std::string::npos, hir, error))
                    Fail("Error processing rule " + rule_name + ": " + error);
                return ExpandRegex(hir, next_state_id);
            }
            case Rule::Kind::String: {
                std::vector<uint32_t> chars = DecodeUtf8(rule.value);
                for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
                    PushAdvance(CharacterSet{}.AddChar(*it), next_state_id);
                    next_state_id = nfa.LastStateId();
                }
                return !rule.value.empty();
            }
            case Rule::Kind::Choice: {
                std::vector<uint32_t> alternative_state_ids;
                for (const Rule &element : rule.members) {
                    if (ExpandRule(element, next_state_id)) alternative_state_ids.push_back(nfa.LastStateId());
                    else alternative_state_ids.push_back(next_state_id);
                }
                PushAlternatives(alternative_state_ids);
                return true;
            }
            case Rule::Kind::Seq: {
                bool result = false;
                for (auto it = rule.members.rbegin(); it != rule.members.rend(); ++it) {
                    if (ExpandRule(*it, next_state_id)) result = true;
                    next_state_id = nfa.LastStateId();
                }
                return result;
            }
            case Rule::Kind::Repeat: {
                PushPlaceholder();
                uint32_t split_state_id = nfa.LastStateId();
                if (ExpandRule(rule.Inner(), split_state_id)) {
                    SetSplit(split_state_id, nfa.LastStateId(), next_state_id);
                    return true;
                }
                return false;
            }
            case Rule::Kind::Metadata: {
                bool has_precedence = rule.Params().precedence.kind == Precedence::Kind::Integer;
                if (has_precedence) precedence_stack.push_back(rule.Params().precedence.integer);
                bool result = ExpandRule(rule.Inner(), next_state_id);
                if (has_precedence) precedence_stack.pop_back();
                return result;
            }
            case Rule::Kind::Blank:
                return false;
            default:
                Fail("Error processing rule " + rule_name + ": Grammar error: Unexpected rule");
        }
    }

    void PushAlternatives(std::vector<uint32_t> &ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        uint32_t last = nfa.LastStateId();
        ids.erase(std::remove(ids.begin(), ids.end(), last), ids.end());
        for (uint32_t id : ids) PushSplit(id);
    }

    bool ExpandRegex(const regex::Hir &hir, uint32_t next_state_id) {
        using K = regex::Hir::Kind;
        switch (hir.kind) {
            case K::Empty:
                return false;
            case K::Literal: {
                std::vector<uint32_t> chars = DecodeUtf8(hir.literal);
                for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
                    PushAdvance(CharacterSet::FromChar(*it), next_state_id);
                    next_state_id = nfa.LastStateId();
                }
                return true;
            }
            case K::Class: {
                CharacterSet chars;
                for (const auto &r : hir.cls.ranges) chars = chars.AddRange(r.first, r.second);
                // The long s `ſ` comes with an `s` under case folding: dropped.
                if (chars.RangeCount() == 3) {
                    bool all = true;
                    for (const auto &r : chars.CharRanges()) {
                        bool ok = (r.first == 's' && r.second == 's') || (r.first == 'S' && r.second == 'S') ||
                                  (r.first == 0x17F && r.second == 0x17F);
                        if (!ok) all = false;
                    }
                    if (all) chars = chars.Difference(CharacterSet::FromChar(0x17F));
                }
                PushAdvance(std::move(chars), next_state_id);
                return true;
            }
            case K::Look:
                Fail("Error processing rule " + rule_name + ": Regex error: Assertions are not supported");
            case K::Repetition: {
                const regex::Hir &sub = hir.subs[0];
                uint32_t min = hir.min;
                std::optional<uint32_t> max = hir.max;
                if (min == 0 && max == std::optional<uint32_t>(1)) return ExpandZeroOrOne(sub, next_state_id);
                if (min == 1 && !max) return ExpandOneOrMore(sub, next_state_id);
                if (min == 0 && !max) return ExpandZeroOrMore(sub, next_state_id);
                if (max && min == *max) return ExpandCount(sub, min, next_state_id);
                if (!max) {
                    if (ExpandZeroOrMore(sub, next_state_id)) return ExpandCount(sub, min, next_state_id);
                    return false;
                }
                bool result = ExpandCount(sub, min, next_state_id);
                for (uint32_t k = min; k < *max; k++) {
                    if (result) next_state_id = nfa.LastStateId();
                    if (ExpandZeroOrOne(sub, next_state_id)) result = true;
                }
                return result;
            }
            case K::Capture:
                return ExpandRegex(hir.subs[0], next_state_id);
            case K::Concat: {
                bool result = false;
                for (auto it = hir.subs.rbegin(); it != hir.subs.rend(); ++it) {
                    if (ExpandRegex(*it, next_state_id)) {
                        result = true;
                        next_state_id = nfa.LastStateId();
                    }
                }
                return result;
            }
            case K::Alternation: {
                std::vector<uint32_t> ids;
                for (const regex::Hir &h : hir.subs) {
                    if (ExpandRegex(h, next_state_id)) ids.push_back(nfa.LastStateId());
                    else ids.push_back(next_state_id);
                }
                PushAlternatives(ids);
                return true;
            }
        }
        return false;
    }

    bool ExpandOneOrMore(const regex::Hir &hir, uint32_t next_state_id) {
        PushPlaceholder();
        uint32_t split_state_id = nfa.LastStateId();
        if (ExpandRegex(hir, split_state_id)) {
            SetSplit(split_state_id, nfa.LastStateId(), next_state_id);
            return true;
        }
        nfa.states.pop_back();
        return false;
    }

    bool ExpandZeroOrOne(const regex::Hir &hir, uint32_t next_state_id) {
        if (ExpandRegex(hir, next_state_id)) {
            PushSplit(next_state_id);
            return true;
        }
        return false;
    }

    bool ExpandZeroOrMore(const regex::Hir &hir, uint32_t next_state_id) {
        if (ExpandOneOrMore(hir, next_state_id)) {
            PushSplit(next_state_id);
            return true;
        }
        return false;
    }

    bool ExpandCount(const regex::Hir &hir, uint32_t count, uint32_t next_state_id) {
        bool result = false;
        for (uint32_t k = 0; k < count; k++) {
            if (ExpandRegex(hir, next_state_id)) {
                result = true;
                next_state_id = nfa.LastStateId();
            }
        }
        return result;
    }

    void PushPlaceholder() {
        NfaState s;
        s.kind = NfaState::Kind::Accept;
        nfa.states.push_back(std::move(s));
    }

    void SetSplit(uint32_t at, uint32_t left, uint32_t right) {
        NfaState s;
        s.kind = NfaState::Kind::Split;
        s.state_id = left;
        s.right = right;
        nfa.states[at] = std::move(s);
    }

    void PushAdvance(CharacterSet chars, uint32_t state_id) {
        NfaState s;
        s.kind = NfaState::Kind::Advance;
        s.chars = std::move(chars);
        s.state_id = state_id;
        s.precedence = precedence_stack.back();
        s.is_sep = is_sep;
        nfa.states.push_back(std::move(s));
    }

    void PushSplit(uint32_t state_id) {
        uint32_t last = nfa.LastStateId();
        NfaState s;
        s.kind = NfaState::Kind::Split;
        s.state_id = state_id;
        s.right = last;
        nfa.states.push_back(std::move(s));
    }
};

LexicalGrammar ExpandTokens(ExtractedLexicalGrammar grammar) {
    NfaBuilder builder;
    Rule separator_rule = Rule::Blank();
    if (!grammar.separators.empty()) {
        grammar.separators.push_back(Rule::Blank());
        separator_rule = Rule::RepeatOf(Rule::ChoiceOf(std::move(grammar.separators)));
    }
    LexicalGrammar out;
    for (size_t i = 0; i < grammar.variables.size(); i++) {
        Variable &variable = grammar.variables[i];
        if (variable.rule.IsEmpty())
            Fail("The rule `" + variable.name +
                 "` matches the empty string.\nTree-sitter does not support syntactic rules that match the empty "
                 "string\nunless they are used only as the grammar's start rule.\n");
        bool is_immediate_token = variable.rule.kind == Rule::Kind::Metadata && variable.rule.Params().is_main_token;
        builder.is_sep = false;
        builder.rule_name = variable.name;
        NfaState accept;
        accept.kind = NfaState::Kind::Accept;
        accept.variable_index = i;
        accept.precedence = GetCompletionPrecedence(variable.rule);
        builder.nfa.states.push_back(std::move(accept));
        builder.ExpandRule(variable.rule, builder.nfa.LastStateId());
        if (!is_immediate_token) {
            builder.is_sep = true;
            builder.ExpandRule(separator_rule, builder.nfa.LastStateId());
        }
        out.variables.push_back(
            {variable.name, variable.kind, GetImplicitPrecedence(variable.rule), builder.nfa.LastStateId()});
    }
    out.nfa = std::move(builder.nfa);
    return out;
}

// ---- extract_default_aliases.rs ---------------------------------------------------------------

struct SymbolStatus {
    std::vector<std::pair<Alias, size_t>> aliases;
    bool appears_unaliased = false;
};

AliasMap ExtractDefaultAliases(SyntaxGrammar &syntax, const LexicalGrammar &lexical) {
    std::vector<SymbolStatus> terminal(lexical.variables.size()), non_terminal(syntax.variables.size()),
        external(syntax.external_tokens.size());
    auto status_of = [&](const Symbol &s) -> SymbolStatus & {
        switch (s.kind) {
            case SymbolType::External: return external[s.index];
            case SymbolType::NonTerminal: return non_terminal[s.index];
            case SymbolType::Terminal: return terminal[s.index];
            default: Fail("Unexpected end token");
        }
    };
    for (const SyntaxVariable &v : syntax.variables) {
        for (const Production &p : v.productions) {
            for (const ProductionStep &step : p.steps) {
                SymbolStatus &status = status_of(step.symbol);
                if (Contains(syntax.variables_to_inline, step.symbol)) continue;
                if (step.alias) {
                    bool found = false;
                    for (auto &[a, count] : status.aliases) {
                        if (a == *step.alias) {
                            count++;
                            found = true;
                            break;
                        }
                    }
                    if (!found) status.aliases.push_back({*step.alias, 1});
                } else {
                    status.appears_unaliased = true;
                }
            }
        }
    }
    for (const Symbol &s : syntax.extra_symbols) status_of(s).appears_unaliased = true;

    AliasMap result;
    auto process = [&](Symbol symbol, SymbolStatus &status) {
        if (status.appears_unaliased) {
            status.aliases.clear();
            return;
        }
        if (status.aliases.empty()) return;
        // max_by_key (count, -index): the most frequent, the first on ties
        size_t best = 0;
        for (size_t i = 1; i < status.aliases.size(); i++)
            if (status.aliases[i].second > status.aliases[best].second) best = i;
        auto entry = status.aliases[best];
        status.aliases.clear();
        status.aliases.push_back(entry);
        result[symbol] = entry.first;
    };
    for (size_t i = 0; i < terminal.size(); i++) process(Symbol::Terminal(i), terminal[i]);
    for (size_t i = 0; i < non_terminal.size(); i++) process(Symbol::NonTerminal(i), non_terminal[i]);
    for (size_t i = 0; i < external.size(); i++) process(Symbol::External(i), external[i]);

    for (SyntaxVariable &variable : syntax.variables) {
        std::vector<std::pair<size_t, size_t>> to_clear;
        for (size_t i = 0; i < variable.productions.size(); i++) {
            const Production &production = variable.productions[i];
            for (size_t j = 0; j < production.steps.size(); j++) {
                const ProductionStep &step = production.steps[j];
                SymbolStatus &status = status_of(step.symbol);
                if (!step.alias || status.aliases.empty() || !(status.aliases[0].first == *step.alias)) continue;
                bool other_must_use = false;
                for (size_t other_i = 0; other_i < variable.productions.size(); other_i++) {
                    const Production &other = variable.productions[other_i];
                    if (other_i != i && other.steps.size() > j && other.steps[j].alias == step.alias) {
                        auto it = result.find(other.steps[j].symbol);
                        bool same = it != result.end() && it->second == *step.alias;
                        if (!same) {
                            other_must_use = true;
                            break;
                        }
                    }
                }
                if (!other_must_use) to_clear.push_back({i, j});
            }
        }
        for (auto [i, j] : to_clear) variable.productions[i].steps[j].alias.reset();
    }
    return result;
}

// ---- process_inlines.rs ------------------------------------------------------------------------

struct ProductionStepId {
    std::optional<size_t> variable_index;  // none: one of the builder's own productions
    size_t production_index;
    size_t step_index;
    bool operator==(const ProductionStepId &) const = default;
};

struct StepIdHash {
    size_t operator()(const ProductionStepId &id) const {
        return (id.variable_index ? *id.variable_index + 1 : 0) * 1000003 + id.production_index * 1009 + id.step_index;
    }
};

struct InlinedBuilder {
    std::unordered_map<ProductionStepId, std::vector<size_t>, StepIdHash> production_indices_by_step_id;
    std::vector<std::unique_ptr<Production>> productions;

    const Production &ProductionFor(const ProductionStepId &id, const SyntaxGrammar &g) const {
        return id.variable_index ? g.variables[*id.variable_index].productions[id.production_index] : *productions[id.production_index];
    }
    const ProductionStep *StepFor(const ProductionStepId &id, const SyntaxGrammar &g) const {
        const Production &p = ProductionFor(id, g);
        return id.step_index < p.steps.size() ? &p.steps[id.step_index] : nullptr;
    }

    std::vector<size_t> InlineProductionAtStep(const ProductionStepId &step_id, const SyntaxGrammar &grammar) {
        size_t step_index = step_id.step_index;
        std::vector<Production> to_add{ProductionFor(step_id, grammar)};
        size_t i = 0;
        while (i < to_add.size()) {
            if (step_index < to_add[i].steps.size()) {
                Symbol symbol = to_add[i].steps[step_index].symbol;
                if (Contains(grammar.variables_to_inline, symbol)) {
                    Production production = std::move(to_add[i]);
                    std::vector<Production> replacements;
                    for (const Production &p : grammar.variables[symbol.index].productions) {
                        Production np = production;
                        ProductionStep removed = np.steps[step_index];
                        np.steps.erase(np.steps.begin() + static_cast<long>(step_index));
                        np.steps.insert(np.steps.begin() + static_cast<long>(step_index), p.steps.begin(), p.steps.end());
                        auto first = np.steps.begin() + static_cast<long>(step_index);
                        auto last = first + static_cast<long>(p.steps.size());
                        if (removed.alias)
                            for (auto it = first; it != last; ++it) it->alias = removed.alias;
                        if (removed.field_name)
                            for (auto it = first; it != last; ++it) it->field_name = removed.field_name;
                        if (first != last) {
                            ProductionStep &last_inserted = *(last - 1);
                            if (last_inserted.precedence.IsNone()) last_inserted.precedence = removed.precedence;
                            if (!last_inserted.associativity) last_inserted.associativity = removed.associativity;
                        }
                        if (std::abs(p.dynamic_precedence) > std::abs(np.dynamic_precedence))
                            np.dynamic_precedence = p.dynamic_precedence;
                        replacements.push_back(std::move(np));
                    }
                    to_add.erase(to_add.begin() + static_cast<long>(i));
                    to_add.insert(to_add.begin() + static_cast<long>(i), std::make_move_iterator(replacements.begin()),
                                  std::make_move_iterator(replacements.end()));
                    continue;
                }
            }
            i++;
        }
        std::vector<size_t> result;
        for (Production &p : to_add) {
            size_t index = productions.size();
            for (size_t k = 0; k < productions.size(); k++) {
                if (*productions[k] == p) {
                    index = k;
                    break;
                }
            }
            if (index == productions.size()) productions.push_back(std::make_unique<Production>(std::move(p)));
            result.push_back(index);
        }
        auto [it, inserted] = production_indices_by_step_id.emplace(step_id, result);
        return it->second;
    }

    InlinedProductionMap Build(const SyntaxGrammar &grammar) {
        std::vector<ProductionStepId> to_process;
        for (size_t vi = 0; vi < grammar.variables.size(); vi++) {
            for (size_t pi = 0; pi < grammar.variables[vi].productions.size(); pi++) {
                to_process.push_back({vi, pi, 0});
                while (!to_process.empty()) {
                    size_t i = 0;
                    while (i < to_process.size()) {
                        ProductionStepId step_id = to_process[i];
                        if (const ProductionStep *step = StepFor(step_id, grammar)) {
                            if (Contains(grammar.variables_to_inline, step->symbol)) {
                                std::vector<size_t> inlined = InlineProductionAtStep(step_id, grammar);
                                std::vector<ProductionStepId> ids;
                                for (size_t pidx : inlined) ids.push_back({std::nullopt, pidx, step_id.step_index});
                                to_process.erase(to_process.begin() + static_cast<long>(i));
                                to_process.insert(to_process.begin() + static_cast<long>(i), ids.begin(), ids.end());
                            } else {
                                to_process[i] = {step_id.variable_index, step_id.production_index, step_id.step_index + 1};
                                i++;
                            }
                        } else {
                            to_process.erase(to_process.begin() + static_cast<long>(i));
                        }
                    }
                }
            }
        }
        InlinedProductionMap out;
        for (const auto &[step_id, indices] : production_indices_by_step_id) {
            const Production *p = step_id.variable_index
                                      ? &grammar.variables[*step_id.variable_index].productions[step_id.production_index]
                                      : productions[step_id.production_index].get();
            out.production_map[{p, static_cast<uint32_t>(step_id.step_index)}] = indices;
        }
        out.productions = std::move(productions);
        return out;
    }
};

InlinedProductionMap ProcessInlines(const SyntaxGrammar &grammar, const LexicalGrammar &lexical) {
    for (const Symbol &s : grammar.variables_to_inline) {
        if (s.IsExternal()) Fail("External token `" + grammar.external_tokens[s.index].name + "` cannot be inlined");
        if (s.IsTerminal()) Fail("Token `" + lexical.variables[s.index].name + "` cannot be inlined");
        if (s.IsNonTerminal() && s.index == 0)
            Fail("Rule `" + grammar.variables[s.index].name + "` cannot be inlined because it is the first rule");
    }
    return InlinedBuilder{}.Build(grammar);
}

}  // namespace

PreparedGrammar PrepareGrammar(const InputGrammar &input) {
    ValidatePrecedences(input);
    ValidateIndirectRecursion(input);
    InternedGrammar interned = InternSymbols(input);
    auto [extracted_syntax, extracted_lexical] = ExtractTokens(std::move(interned));
    ExpandRepeats(extracted_syntax);
    PreparedGrammar out;
    out.syntax = FlattenGrammar(std::move(extracted_syntax));
    out.lexical = ExpandTokens(std::move(extracted_lexical));
    out.simple_aliases = ExtractDefaultAliases(out.syntax, out.lexical);
    // The inlined productions point into the syntax grammar's productions,
    // which keep their addresses when `out` is moved.
    out.inlines = ProcessInlines(out.syntax, out.lexical);
    return out;
}

}  // namespace tsgen
