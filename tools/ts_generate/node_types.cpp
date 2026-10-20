// node-types.json and the per-variable child summaries: a port of
// node_types.rs.
#include <algorithm>
#include <set>

#include "generate.h"
#include "rust_sort.h"

namespace tsgen {

void ChildQuantity::Append(ChildQuantity other) {
    if (other.exists) {
        if (exists || other.multiple) multiple = true;
        if (other.required) required = true;
        exists = true;
    }
}

bool ChildQuantity::Union(ChildQuantity other) {
    bool result = false;
    if (!exists && other.exists) {
        result = true;
        exists = true;
    }
    if (required && !other.required) {
        result = true;
        required = false;
    }
    if (!multiple && other.multiple) {
        result = true;
        multiple = true;
    }
    return result;
}

namespace {

VariableType VariableTypeForChildType(const ChildType &t, const SyntaxGrammar &syntax, const LexicalGrammar &lexical) {
    if (t.aliased) return t.alias.Kind();
    const Symbol &s = t.symbol;
    if (Contains(syntax.supertype_symbols, s)) return VariableType::Named;
    if (Contains(syntax.variables_to_inline, s)) return VariableType::Hidden;
    switch (s.kind) {
        case SymbolType::NonTerminal: return syntax.variables[s.index].kind;
        case SymbolType::Terminal: return lexical.variables[s.index].kind;
        case SymbolType::External: return syntax.external_tokens[s.index].kind;
        default: return VariableType::Hidden;
    }
}

// extend_sorted: inserts values in order until the first that is new.
// (`any` short-circuits: at most one value is added per call.)
bool ExtendSorted(std::vector<ChildType> &vec, const std::vector<ChildType> &values) {
    for (const ChildType &value : values) {
        auto it = std::lower_bound(vec.begin(), vec.end(), value);
        if (it == vec.end() || !(*it == value)) {
            vec.insert(it, value);
            return true;
        }
    }
    return false;
}

bool ExtendSortedOne(std::vector<ChildType> &vec, const ChildType &value) { return ExtendSorted(vec, {value}); }

}  // namespace

std::vector<VariableInfo> GetVariableInfo(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                          const AliasMap &default_aliases) {
    auto child_type_is_visible = [&](const ChildType &t) {
        return VariableTypeForChildType(t, syntax, lexical) >= VariableType::Anonymous;
    };
    auto child_type_is_named = [&](const ChildType &t) {
        return VariableTypeForChildType(t, syntax, lexical) == VariableType::Named;
    };
    bool did_change = true;
    bool all_initialized = false;
    std::vector<VariableInfo> result(syntax.variables.size());
    while (did_change) {
        did_change = false;
        for (size_t i = 0; i < syntax.variables.size(); i++) {
            VariableInfo variable_info = result[i];
            for (const Production &production : syntax.variables[i].productions) {
                std::map<std::string, ChildQuantity> production_field_quantities;
                ChildQuantity production_children_quantity = ChildQuantity::Zero();
                ChildQuantity production_children_without_fields_quantity = ChildQuantity::Zero();
                bool production_has_uninitialized_invisible_children = false;
                if (production.steps.size() > 1) variable_info.has_multi_step_production = true;
                for (const ProductionStep &step : production.steps) {
                    Symbol child_symbol = step.symbol;
                    ChildType child_type;
                    if (step.alias) {
                        child_type.aliased = true;
                        child_type.alias = *step.alias;
                    } else if (auto it = default_aliases.find(step.symbol); it != default_aliases.end()) {
                        child_type.aliased = true;
                        child_type.alias = it->second;
                    } else {
                        child_type.symbol = child_symbol;
                    }
                    bool child_is_hidden = !child_type_is_visible(child_type) && !Contains(syntax.supertype_symbols, child_symbol);
                    did_change |= ExtendSortedOne(variable_info.children.types, child_type);
                    if (!child_is_hidden) production_children_quantity.Append(ChildQuantity::One());
                    if (step.field_name) {
                        FieldInfo &field_info = variable_info.fields[*step.field_name];
                        did_change |= ExtendSortedOne(field_info.types, child_type);
                        auto [qit, inserted] = production_field_quantities.emplace(*step.field_name, ChildQuantity::Zero());
                        ChildQuantity &production_field_quantity = qit->second;
                        if (child_is_hidden && child_symbol.IsNonTerminal()) {
                            const VariableInfo &child_info = result[child_symbol.index];
                            did_change |= ExtendSorted(field_info.types, child_info.children.types);
                            production_field_quantity.Append(child_info.children.quantity);
                        } else {
                            production_field_quantity.Append(ChildQuantity::One());
                        }
                    } else if (child_type_is_named(child_type)) {
                        production_children_without_fields_quantity.Append(ChildQuantity::One());
                        did_change |= ExtendSortedOne(variable_info.children_without_fields.types, child_type);
                    }
                    if (child_is_hidden && child_symbol.IsNonTerminal()) {
                        const VariableInfo &child_info = result[child_symbol.index];
                        if (child_info.has_multi_step_production) variable_info.has_multi_step_production = true;
                        for (const auto &[field_name, child_field_info] : child_info.fields) {
                            production_field_quantities.emplace(field_name, ChildQuantity::Zero())
                                .first->second.Append(child_field_info.quantity);
                            did_change |= ExtendSorted(variable_info.fields[field_name].types, child_field_info.types);
                        }
                        production_children_quantity.Append(child_info.children.quantity);
                        did_change |= ExtendSorted(variable_info.children.types, child_info.children.types);
                        if (!step.field_name) {
                            const FieldInfo &grandchildren = child_info.children_without_fields;
                            if (!grandchildren.types.empty()) {
                                production_children_without_fields_quantity.Append(grandchildren.quantity);
                                did_change |= ExtendSorted(variable_info.children_without_fields.types, grandchildren.types);
                            }
                        }
                    }
                    if (child_symbol.index >= i && !all_initialized) production_has_uninitialized_invisible_children = true;
                }
                if (!production_has_uninitialized_invisible_children) {
                    did_change |= variable_info.children.quantity.Union(production_children_quantity);
                    did_change |=
                        variable_info.children_without_fields.quantity.Union(production_children_without_fields_quantity);
                    for (auto &[field_name, info] : variable_info.fields) {
                        auto it = production_field_quantities.find(field_name);
                        did_change |= info.quantity.Union(it == production_field_quantities.end() ? ChildQuantity::Zero() : it->second);
                    }
                }
            }
            result[i] = std::move(variable_info);
        }
        all_initialized = true;
    }
    for (const Symbol &s : syntax.supertype_symbols)
        if (result[s.index].has_multi_step_production)
            Fail("Grammar error: Supertype symbols must always have a single visible child, but `" +
                 syntax.variables[s.index].name + "` can have multiple");
    auto retain_visible = [&](std::vector<ChildType> &types) {
        std::vector<ChildType> kept;
        for (ChildType &t : types)
            if (child_type_is_visible(t)) kept.push_back(std::move(t));
        types = std::move(kept);
    };
    for (const Symbol &s : syntax.supertype_symbols) retain_visible(result[s.index].children.types);
    for (VariableInfo &info : result) {
        for (auto &[name, field] : info.fields) retain_visible(field.types);
        for (auto it = info.fields.begin(); it != info.fields.end();) {
            if (it->second.types.empty()) it = info.fields.erase(it);
            else ++it;
        }
        retain_visible(info.children_without_fields.types);
    }
    return result;
}

namespace {

// HashSet<Option<Alias>> per symbol; ordered here, as the order never
// matters to the output.
using AliasSet = std::set<std::optional<Alias>>;

std::map<Symbol, AliasSet> GetAliasesBySymbol(const SyntaxGrammar &syntax, const AliasMap &default_aliases) {
    std::map<Symbol, AliasSet> result;
    for (const auto &[symbol, alias] : default_aliases) result[symbol] = {alias};
    for (const Symbol &extra : syntax.extra_symbols)
        if (!default_aliases.count(extra)) result[extra].insert(std::nullopt);
    for (const SyntaxVariable &v : syntax.variables) {
        for (const Production &p : v.productions) {
            for (const ProductionStep &step : p.steps) {
                std::optional<Alias> alias = step.alias;
                if (!alias)
                    if (auto it = default_aliases.find(step.symbol); it != default_aliases.end()) alias = it->second;
                result[step.symbol].insert(alias);
            }
        }
    }
    result[Symbol::NonTerminal(0)] = {std::nullopt};
    return result;
}

struct NodeTypeJSON {
    std::string kind;
    bool named = false;
    auto operator<=>(const NodeTypeJSON &) const = default;
    bool operator==(const NodeTypeJSON &) const = default;
};

struct FieldInfoJSON {
    bool multiple = false;
    bool required = true;
    std::vector<NodeTypeJSON> types;
    bool operator==(const FieldInfoJSON &) const = default;
};

struct NodeInfoJSON {
    std::string kind;
    bool named = false;
    bool root = false;
    bool extra = false;
    std::optional<std::map<std::string, FieldInfoJSON>> fields;
    std::optional<FieldInfoJSON> children;
    std::optional<std::vector<NodeTypeJSON>> subtypes;
    bool operator==(const NodeInfoJSON &) const = default;
};

JsonValue NodeTypeToJson(const NodeTypeJSON &t) {
    JsonValue v = JsonValue::Obj();
    v.Set("type", JsonValue::Str(t.kind));
    v.Set("named", JsonValue::Bool(t.named));
    return v;
}

JsonValue FieldInfoToJson(const FieldInfoJSON &f) {
    JsonValue v = JsonValue::Obj();
    v.Set("multiple", JsonValue::Bool(f.multiple));
    v.Set("required", JsonValue::Bool(f.required));
    JsonValue types = JsonValue::Arr();
    for (const NodeTypeJSON &t : f.types) types.array.push_back(NodeTypeToJson(t));
    v.Set("types", std::move(types));
    return v;
}

void SortDedup(std::vector<NodeTypeJSON> &v) {
    std::sort(v.begin(), v.end());  // sort_unstable on a total order with no equal distinct values
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace

std::map<Symbol, std::vector<ChildType>> GetSupertypeSymbolMap(const SyntaxGrammar &syntax, const AliasMap &,
                                                               const std::vector<VariableInfo> &variable_info) {
    std::map<Symbol, std::vector<ChildType>> result;
    for (size_t i = 0; i < variable_info.size(); i++)
        if (Contains(syntax.supertype_symbols, Symbol::NonTerminal(i))) result[Symbol::NonTerminal(i)] = variable_info[i].children.types;
    return result;
}

JsonValue GenerateNodeTypesJson(const SyntaxGrammar &syntax, const LexicalGrammar &lexical,
                                const AliasMap &default_aliases, const std::vector<VariableInfo> &variable_info) {
    std::map<std::string, NodeInfoJSON> node_types_json;
    auto child_type_to_node_type = [&](const ChildType &ct) -> NodeTypeJSON {
        if (ct.aliased) return {ct.alias.value, ct.alias.is_named};
        if (auto it = default_aliases.find(ct.symbol); it != default_aliases.end()) return {it->second.value, it->second.is_named};
        switch (ct.symbol.kind) {
            case SymbolType::NonTerminal: {
                const SyntaxVariable &v = syntax.variables[ct.symbol.index];
                return {v.name, v.kind != VariableType::Anonymous};
            }
            case SymbolType::Terminal: {
                const LexicalVariable &v = lexical.variables[ct.symbol.index];
                return {v.name, v.kind != VariableType::Anonymous};
            }
            case SymbolType::External: {
                const ExternalToken &v = syntax.external_tokens[ct.symbol.index];
                return {v.name, v.kind != VariableType::Anonymous};
            }
            default:
                Fail("Unexpected symbol type");
        }
    };
    auto populate_field_info_json = [&](FieldInfoJSON &json, const FieldInfo &info) {
        if (info.types.empty()) {
            json.required = false;
        } else {
            json.multiple |= info.quantity.multiple;
            json.required &= info.quantity.required;
            for (const ChildType &t : info.types) json.types.push_back(child_type_to_node_type(t));
            SortDedup(json.types);
        }
    };

    std::map<Symbol, AliasSet> aliases_by_symbol = GetAliasesBySymbol(syntax, default_aliases);
    const AliasSet empty;
    auto aliases_of = [&](const Symbol &s) -> const AliasSet & {
        auto it = aliases_by_symbol.find(s);
        return it == aliases_by_symbol.end() ? empty : it->second;
    };

    std::set<std::string> extra_names;
    for (const Symbol &symbol : syntax.extra_symbols) {
        for (const std::optional<Alias> &alias : aliases_of(symbol)) {
            if (alias) {
                extra_names.insert(alias->value);
                continue;
            }
            switch (symbol.kind) {
                case SymbolType::NonTerminal: extra_names.insert(syntax.variables[symbol.index].name); break;
                case SymbolType::Terminal: extra_names.insert(lexical.variables[symbol.index].name); break;
                case SymbolType::External: extra_names.insert(syntax.external_tokens[symbol.index].name); break;
                default: break;
            }
        }
    }

    std::vector<std::pair<NodeTypeJSON, std::vector<NodeTypeJSON>>> subtype_map;
    for (size_t i = 0; i < variable_info.size(); i++) {
        const VariableInfo &info = variable_info[i];
        Symbol symbol = Symbol::NonTerminal(i);
        const SyntaxVariable &variable = syntax.variables[i];
        if (Contains(syntax.supertype_symbols, symbol)) {
            auto [it, inserted] = node_types_json.emplace(variable.name, NodeInfoJSON{});
            NodeInfoJSON &node = it->second;
            if (inserted) {
                node.kind = variable.name;
                node.named = true;
                node.extra = extra_names.count(variable.name) != 0;
            }
            std::vector<NodeTypeJSON> subtypes;
            for (const ChildType &t : info.children.types) subtypes.push_back(child_type_to_node_type(t));
            SortDedup(subtypes);
            subtype_map.push_back({NodeTypeJSON{node.kind, true}, subtypes});
            node.subtypes = subtypes;
        } else if (!Contains(syntax.variables_to_inline, symbol)) {
            for (const std::optional<Alias> &alias : aliases_of(symbol)) {
                std::string kind;
                bool is_named;
                if (alias) {
                    kind = alias->value;
                    is_named = alias->is_named;
                } else if (IsVisible(variable.kind)) {
                    kind = variable.name;
                    is_named = variable.kind == VariableType::Named;
                } else {
                    continue;
                }
                auto [it, inserted] = node_types_json.emplace(kind, NodeInfoJSON{});
                bool node_type_existed = !inserted;
                NodeInfoJSON &node = it->second;
                if (inserted) {
                    node.kind = kind;
                    node.named = is_named;
                    node.root = i == 0;
                    node.extra = extra_names.count(kind) != 0;
                    node.fields = std::map<std::string, FieldInfoJSON>{};
                }
                auto &fields_json = *node.fields;
                for (const auto &[new_field, field_info] : info.fields) {
                    auto [fit, finserted] = fields_json.emplace(new_field, FieldInfoJSON{});
                    if (finserted && node_type_existed) fit->second.required = false;
                    populate_field_info_json(fit->second, field_info);
                }
                for (auto &[existing_field, field_json] : fields_json)
                    if (!info.fields.count(existing_field)) field_json.required = false;
                if (!node.children) node.children = FieldInfoJSON{};
                populate_field_info_json(*node.children, info.children_without_fields);
            }
        }
    }

    // Topological sort of the supertypes, subtypes first (topological-sort's
    // pop_all, each batch sorted).
    std::vector<std::string> sorted_kinds;
    {
        std::map<std::string, size_t> num_prec;
        std::map<std::string, std::set<std::string>> succ;
        auto add_node = [&](const std::string &k) { num_prec.emplace(k, 0); };
        for (const auto &[supertype, subtypes] : subtype_map) {
            for (const NodeTypeJSON &subtype : subtypes) {
                const std::string &prec = subtype.kind, &s = supertype.kind;
                add_node(prec);
                add_node(s);
                if (succ[prec].insert(s).second) num_prec[s]++;
            }
        }
        while (true) {
            std::vector<std::string> next;
            for (const auto &[k, n] : num_prec)
                if (n == 0) next.push_back(k);
            if (next.empty()) {
                if (num_prec.empty()) break;
                std::string message = "Dependency cycle detected in node types:";
                std::vector<std::string> items;
                for (const auto &[k, n] : num_prec) items.push_back(k);
                for (size_t i = 0; i < items.size(); i++) message += " " + items[i] + (i + 1 < items.size() ? "," : "");
                Fail(message);
            }
            for (const std::string &k : next) {
                num_prec.erase(k);
                for (const std::string &s : succ[k]) num_prec[s]--;
            }
            sorted_kinds.insert(sorted_kinds.end(), next.begin(), next.end());
        }
    }
    auto kind_index = [&](const std::string &kind) {
        return std::find(sorted_kinds.begin(), sorted_kinds.end(), kind) - sorted_kinds.begin();
    };
    std::stable_sort(subtype_map.begin(), subtype_map.end(),
                     [&](const auto &a, const auto &b) { return kind_index(a.first.kind) < kind_index(b.first.kind); });

    auto process_supertypes = [&](FieldInfoJSON &info) {
        for (const auto &[supertype, subtypes] : subtype_map) {
            if (std::find(info.types.begin(), info.types.end(), supertype) != info.types.end()) {
                std::vector<NodeTypeJSON> kept;
                for (NodeTypeJSON &t : info.types)
                    if (std::find(subtypes.begin(), subtypes.end(), t) == subtypes.end()) kept.push_back(std::move(t));
                info.types = std::move(kept);
            }
        }
    };
    for (auto &[name, node] : node_types_json) {
        if (node.children && node.children->types.empty()) node.children.reset();
        if (node.children) process_supertypes(*node.children);
        if (node.fields)
            for (auto &[fname, field] : *node.fields) process_supertypes(field);
    }

    std::vector<NodeInfoJSON> anonymous_node_types;
    auto add_token = [&](const std::string &name, VariableType kind) {
        if (kind == VariableType::Named) {
            auto [it, inserted] = node_types_json.emplace(name, NodeInfoJSON{});
            NodeInfoJSON &node = it->second;
            if (inserted) {
                node.kind = name;
                node.named = true;
                node.extra = extra_names.count(name) != 0;
            }
            if (node.children) node.children->required = false;
            if (node.fields)
                for (auto &[fname, field] : *node.fields) field.required = false;
        } else if (kind == VariableType::Anonymous) {
            NodeInfoJSON node;
            node.kind = name;
            node.extra = extra_names.count(name) != 0;
            anonymous_node_types.push_back(std::move(node));
        }
    };
    for (size_t i = 0; i < lexical.variables.size(); i++)
        for (const std::optional<Alias> &alias : aliases_of(Symbol::Terminal(i))) {
            if (alias) add_token(alias->value, alias->Kind());
            else add_token(lexical.variables[i].name, lexical.variables[i].kind);
        }
    for (size_t i = 0; i < syntax.external_tokens.size(); i++)
        for (const std::optional<Alias> &alias : aliases_of(Symbol::External(i))) {
            if (alias) add_token(alias->value, alias->Kind());
            else add_token(syntax.external_tokens[i].name, syntax.external_tokens[i].kind);
        }

    std::vector<NodeInfoJSON> result;
    for (auto &[name, node] : node_types_json) result.push_back(std::move(node));
    for (NodeInfoJSON &node : anonymous_node_types) result.push_back(std::move(node));
    // sort_unstable_by on NodeInfoJSON (112 bytes, not Copy); equal keys
    // can differ only in `named`, so the exact algorithm decides.
    RustSortUnstable(result, RustSortType{112, false}, [](const NodeInfoJSON &a, const NodeInfoJSON &b) {
        bool as = a.subtypes.has_value(), bs = b.subtypes.has_value();
        if (as != bs) return as;  // b.subtypes.is_some().cmp(&a.subtypes.is_some())
        bool a_leaf = !a.children && !a.fields, b_leaf = !b.children && !b.fields;
        if (a_leaf != b_leaf) return !a_leaf;
        return a.kind < b.kind;
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());

    JsonValue out = JsonValue::Arr();
    for (const NodeInfoJSON &node : result) {
        JsonValue v = JsonValue::Obj();
        v.Set("type", JsonValue::Str(node.kind));
        v.Set("named", JsonValue::Bool(node.named));
        if (node.root) v.Set("root", JsonValue::Bool(true));
        if (node.extra) v.Set("extra", JsonValue::Bool(true));
        if (node.fields) {
            JsonValue fields = JsonValue::Obj();
            for (const auto &[fname, field] : *node.fields) fields.Set(fname, FieldInfoToJson(field));
            v.Set("fields", std::move(fields));
        }
        if (node.children) v.Set("children", FieldInfoToJson(*node.children));
        if (node.subtypes) {
            JsonValue subtypes = JsonValue::Arr();
            for (const NodeTypeJSON &t : *node.subtypes) subtypes.array.push_back(NodeTypeToJson(t));
            v.Set("subtypes", std::move(subtypes));
        }
        out.array.push_back(std::move(v));
    }
    return out;
}

}  // namespace tsgen
