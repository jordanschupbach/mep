// grammar.json -> InputGrammar: a port of parse_grammar.rs.
#include <set>

#include "generate.h"

namespace tsgen {

namespace {

const JsonValue &Field(const JsonValue &obj, const char *key) {
    const JsonValue *v = obj.Get(key);
    if (!v) Fail(std::string("grammar.json: rule is missing `") + key + "`");
    return *v;
}

const std::string &Str(const JsonValue &obj, const char *key) {
    const JsonValue &v = Field(obj, key);
    if (v.type != JsonValue::Type::String) Fail(std::string("grammar.json: `") + key + "` must be a string");
    return v.string;
}

Precedence PrecedenceValue(const JsonValue &v) {
    if (v.type == JsonValue::Type::Number) return Precedence::Int(static_cast<int32_t>(v.AsInt()));
    if (v.type == JsonValue::Type::String) return Precedence::Named(v.string);
    Fail("grammar.json: a precedence must be a number or a string");
}

Rule ParseRule(const JsonValue &json, bool is_token);

std::vector<Rule> ParseMembers(const JsonValue &json, bool is_token) {
    std::vector<Rule> result;
    for (const JsonValue &m : Field(json, "members").array) result.push_back(ParseRule(m, is_token));
    return result;
}

Rule ParseRule(const JsonValue &json, bool is_token) {
    const std::string &type = Str(json, "type");
    if (type == "ALIAS") {
        Rule content = ParseRule(Field(json, "content"), is_token);
        std::string value = Str(json, "value");
        bool named = Field(json, "named").boolean;
        return AddMetadata(std::move(content), [&](MetadataParams &p) { p.alias = Alias{value, named}; });
    }
    if (type == "BLANK") return Rule::Blank();
    if (type == "STRING") return Rule::String(Str(json, "value"));
    if (type == "PATTERN") {
        std::string flags;
        const JsonValue *f = json.Get("flags");
        if (f && f->type == JsonValue::Type::String) {
            for (char c : f->string) {
                if (c == 'i') flags += c;
                else if (c != 'u' && c != 'v') std::fprintf(stderr, "Warning: unsupported flag %c\n", c);
            }
        }
        return Rule::Pattern(Str(json, "value"), flags);
    }
    if (type == "SYMBOL") {
        const std::string &name = Str(json, "name");
        if (is_token) Fail("Grammar Error: Unexpected rule `" + name + "` in `token()` call");
        return Rule::Named(name);
    }
    if (type == "CHOICE") return Rule::ChoiceOf(ParseMembers(json, is_token));
    if (type == "FIELD") {
        Rule content = ParseRule(Field(json, "content"), is_token);
        std::string name = Str(json, "name");
        return AddMetadata(std::move(content), [&](MetadataParams &p) { p.field_name = name; });
    }
    if (type == "SEQ") return Rule::SeqOf(ParseMembers(json, is_token));
    if (type == "REPEAT1") return Rule::RepeatOf(ParseRule(Field(json, "content"), is_token));
    if (type == "REPEAT") {
        std::vector<Rule> choices;
        choices.push_back(Rule::RepeatOf(ParseRule(Field(json, "content"), is_token)));
        choices.push_back(Rule::Blank());
        return Rule::ChoiceOf(std::move(choices));
    }
    if (type == "PREC" || type == "PREC_LEFT" || type == "PREC_RIGHT") {
        Rule content = ParseRule(Field(json, "content"), is_token);
        Precedence value = PrecedenceValue(Field(json, "value"));
        std::optional<Associativity> assoc;
        if (type == "PREC_LEFT") assoc = Associativity::Left;
        if (type == "PREC_RIGHT") assoc = Associativity::Right;
        return AddMetadata(std::move(content), [&](MetadataParams &p) {
            if (assoc) p.associativity = assoc;
            p.precedence = value;
        });
    }
    if (type == "PREC_DYNAMIC") {
        Rule content = ParseRule(Field(json, "content"), is_token);
        int32_t value = static_cast<int32_t>(Field(json, "value").AsInt());
        return AddMetadata(std::move(content), [&](MetadataParams &p) { p.dynamic_precedence = value; });
    }
    if (type == "RESERVED") {
        Rule content = ParseRule(Field(json, "content"), is_token);
        return Rule::ReservedOf(std::move(content), Str(json, "context_name"));
    }
    if (type == "TOKEN") {
        return AddMetadata(ParseRule(Field(json, "content"), true), [](MetadataParams &p) { p.is_token = true; });
    }
    if (type == "IMMEDIATE_TOKEN") {
        return AddMetadata(ParseRule(Field(json, "content"), is_token), [](MetadataParams &p) {
            p.is_token = true;
            p.is_main_token = true;
        });
    }
    Fail("grammar.json: unknown rule type `" + type + "`");
}

bool RuleIsReferenced(const Rule &rule, const std::string &target, bool is_external) {
    switch (rule.kind) {
        case Rule::Kind::NamedSymbol:
            return rule.value == target && !is_external;
        case Rule::Kind::Choice:
        case Rule::Kind::Seq:
            for (const Rule &r : rule.members)
                if (RuleIsReferenced(r, target, false)) return true;
            return false;
        case Rule::Kind::Metadata:
        case Rule::Kind::Reserved:
            return RuleIsReferenced(rule.Inner(), target, is_external);
        case Rule::Kind::Repeat:
            return RuleIsReferenced(rule.Inner(), target, false);
        default:
            return false;
    }
}

bool VariableIsUsed(const std::vector<std::pair<std::string, Rule>> &rules, const std::vector<Rule> &extras,
                    const std::vector<Rule> &externals, const std::string &target, std::set<std::string> &in_progress) {
    if (target == rules.front().first) return true;
    for (const Rule &r : extras)
        if (RuleIsReferenced(r, target, false)) return true;
    for (const Rule &r : externals)
        if (RuleIsReferenced(r, target, true)) return true;
    in_progress.insert(target);
    bool result = false;
    for (const auto &[name, rule] : rules) {
        if (name == target) continue;
        if (!RuleIsReferenced(rule, target, false) || in_progress.count(name)) continue;
        if (VariableIsUsed(rules, extras, externals, name, in_progress)) {
            result = true;
            break;
        }
    }
    in_progress.erase(target);
    return result;
}

template <class T, class Pred>
void Retain(std::vector<T> &v, Pred keep) {
    std::vector<T> out;
    for (T &x : v)
        if (keep(x)) out.push_back(std::move(x));
    v = std::move(out);
}

}  // namespace

InputGrammar ParseGrammar(const JsonValue &json) {
    if (json.type != JsonValue::Type::Object) Fail("grammar.json must hold an object");
    InputGrammar g;
    g.name = Str(json, "name");
    const JsonValue *rules_json = json.Get("rules");
    if (!rules_json || rules_json->type != JsonValue::Type::Object) Fail("grammar.json: missing `rules`");
    auto list = [&](const char *key) -> const std::vector<JsonValue> * {
        const JsonValue *v = json.Get(key);
        if (!v || v->type != JsonValue::Type::Array) return nullptr;
        return &v->array;
    };

    std::vector<Rule> extra_symbols;
    if (auto *extras = list("extras")) {
        for (const JsonValue &e : *extras) {
            Rule rule = ParseRule(e, false);
            if (rule.kind == Rule::Kind::String && rule.value.empty())
                Fail("Rules in the `extras` array must not contain empty strings");
            extra_symbols.push_back(std::move(rule));
        }
    }
    std::vector<Rule> external_tokens;
    if (auto *externals = list("externals"))
        for (const JsonValue &e : *externals) external_tokens.push_back(ParseRule(e, false));

    std::vector<std::vector<PrecedenceEntry>> precedence_orderings;
    if (auto *precs = list("precedences")) {
        for (const JsonValue &l : *precs) {
            std::vector<PrecedenceEntry> ordering;
            for (const JsonValue &entry : l.array) {
                const std::string &type = Str(entry, "type");
                if (type == "STRING") ordering.push_back({PrecedenceEntry::Kind::Name, Str(entry, "value")});
                else if (type == "SYMBOL") ordering.push_back({PrecedenceEntry::Kind::Symbol, Str(entry, "name")});
                else Fail("Invalid rule in precedences array. Only strings and symbols are allowed");
            }
            precedence_orderings.push_back(std::move(ordering));
        }
    }

    std::vector<std::vector<std::string>> conflicts;
    if (auto *c = list("conflicts")) {
        for (const JsonValue &l : *c) {
            std::vector<std::string> names;
            for (const JsonValue &n : l.array) names.push_back(n.string);
            conflicts.push_back(std::move(names));
        }
    }
    std::vector<std::string> supertypes, inlines;
    if (auto *s = list("supertypes"))
        for (const JsonValue &n : *s) supertypes.push_back(n.string);
    if (auto *s = list("inline"))
        for (const JsonValue &n : *s) inlines.push_back(n.string);
    std::optional<std::string> word;
    if (const JsonValue *w = json.Get("word"); w && w->type == JsonValue::Type::String) word = w->string;

    std::vector<std::pair<std::string, Rule>> rules;
    for (const auto &[name, value] : rules_json->object) rules.emplace_back(name, ParseRule(value, false));
    if (rules.empty()) Fail("grammar.json: `rules` is empty");

    std::set<std::string> in_progress;
    for (const auto &[name, rule] : rules) {
        if ((!word || *word != name) && !VariableIsUsed(rules, extra_symbols, external_tokens, name, in_progress)) {
            Retain(conflicts, [&](const std::vector<std::string> &r) {
                for (const std::string &n : r)
                    if (n == name) return false;
                return true;
            });
            Retain(supertypes, [&](const std::string &r) { return r != name; });
            Retain(inlines, [&](const std::string &r) { return r != name; });
            Retain(extra_symbols, [&](const Rule &r) { return !RuleIsReferenced(r, name, true); });
            Retain(external_tokens, [&](const Rule &r) { return !RuleIsReferenced(r, name, true); });
            Retain(precedence_orderings, [&](const std::vector<PrecedenceEntry> &r) {
                for (const PrecedenceEntry &e : r)
                    if (e.kind == PrecedenceEntry::Kind::Symbol && e.value == name) return false;
                return true;
            });
            continue;
        }
        g.variables.push_back(Variable{name, VariableType::Named, rule});
    }

    if (const JsonValue *reserved = json.Get("reserved"); reserved && reserved->type == JsonValue::Type::Object) {
        for (const auto &[name, values] : reserved->object) {
            if (values.type != JsonValue::Type::Array) Fail("Reserved word sets must be arrays");
            ReservedWordContext<Rule> context{name, {}};
            for (const JsonValue &v : values.array) context.reserved_words.push_back(ParseRule(v, false));
            g.reserved_words.push_back(std::move(context));
        }
    }

    g.word_token = word;
    g.expected_conflicts = std::move(conflicts);
    g.supertype_symbols = std::move(supertypes);
    g.variables_to_inline = std::move(inlines);
    g.precedence_orderings = std::move(precedence_orderings);
    g.extra_symbols = std::move(extra_symbols);
    g.external_tokens = std::move(external_tokens);
    return g;
}

}  // namespace tsgen
