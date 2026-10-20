// parser.c: a port of render.rs, line for line -- its text is the output.
#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_set>

#include "build_tables.h"

namespace tsgen {

namespace {

constexpr size_t kSmallStateThreshold = 64;
constexpr size_t kAbiWithReservedWords = 15;
constexpr size_t kLargeCharacterRangeCount = 8;
constexpr const char *kBuildVersion = "0.25.10";

std::string Utf8(uint32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

std::vector<uint32_t> Chars(const std::string &s) {
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

std::string Hex(uint32_t v, int width) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*x", width, v);
    return buf;
}

std::string Bool(bool b) { return b ? "true" : "false"; }

std::string SanitizeIdentifier(const std::string &name) {
    static const std::pair<uint32_t, const char *> kSpecial[] = {
        {'~', "TILDE"},   {'`', "BQUOTE"},  {'!', "BANG"},    {'@', "AT"},     {'#', "POUND"},  {'$', "DOLLAR"},
        {'%', "PERCENT"}, {'^', "CARET"},   {'&', "AMP"},     {'*', "STAR"},   {'(', "LPAREN"}, {')', "RPAREN"},
        {'-', "DASH"},    {'+', "PLUS"},    {'=', "EQ"},      {'{', "LBRACE"}, {'}', "RBRACE"}, {'[', "LBRACK"},
        {']', "RBRACK"},  {'\\', "BSLASH"}, {'|', "PIPE"},    {':', "COLON"},  {';', "SEMI"},   {'"', "DQUOTE"},
        {'\'', "SQUOTE"}, {'<', "LT"},      {'>', "GT"},      {',', "COMMA"},  {'.', "DOT"},    {'?', "QMARK"},
        {'/', "SLASH"},   {'\n', "LF"},     {'\r', "CR"},     {'\t', "TAB"},   {0, "NULL"},     {1, "SOH"},
        {2, "STX"},       {3, "ETX"},       {4, "EOT"},       {5, "ENQ"},      {6, "ACK"},      {7, "BEL"},
        {8, "BS"},        {0x0b, "VTAB"},   {0x0c, "FF"},     {0x0e, "SO"},    {0x0f, "SI"},    {0x10, "DLE"},
        {0x11, "DC1"},    {0x12, "DC2"},    {0x13, "DC3"},    {0x14, "DC4"},   {0x15, "NAK"},   {0x16, "SYN"},
        {0x17, "ETB"},    {0x18, "CAN"},    {0x19, "EM"},     {0x1a, "SUB"},   {0x1b, "ESC"},   {0x1c, "FS"},
        {0x1d, "GS"},     {0x1e, "RS"},     {0x1f, "US"},     {0x7f, "DEL"},   {0xfeff, "BOM"}};
    std::string result;
    for (uint32_t c : Chars(name)) {
        if ((c < 0x80 && std::isalnum(static_cast<int>(c))) || c == '_') {
            result += static_cast<char>(c);
            continue;
        }
        const char *replacement = nullptr;
        if (c == ' ') {
            if (name.size() == 1) replacement = "SPACE";
            else continue;
        }
        if (!replacement)
            for (const auto &[k, r] : kSpecial)
                if (k == c) replacement = r;
        if (!replacement) {
            if (c >= 0x80 && c <= 0xFFFF) result += "u" + Hex(c, 4);
            else if (c >= 0x10000) result += "U" + Hex(c, 8);
            continue;
        }
        if (!result.empty() && result.back() != '_') result += '_';
        result += replacement;
    }
    return result;
}

std::string SanitizeString(const std::string &name) {
    std::string result;
    for (uint32_t c : Chars(name)) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '?': result += "\\?"; break;
            case '\\': result += "\\\\"; break;
            case 0x07: result += "\\a"; break;
            case 0x08: result += "\\b"; break;
            case 0x0b: result += "\\v"; break;
            case 0x0c: result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case 0: result += "\\0"; break;
            default:
                if (c >= 1 && c <= 0x1f) result += "\\x" + Hex(c, 2);
                else if (c >= 0x7f && c <= 0xffff) result += "\\u" + Hex(c, 4);
                else if (c >= 0x10000) result += "\\U" + Hex(c, 8);
                else result += Utf8(c);
        }
    }
    return result;
}

struct EntryHash {
    size_t operator()(const ParseTableEntry &e) const {
        size_t h = e.reusable;
        for (const ParseAction &a : e.actions)
            h = h * 1000003 + static_cast<size_t>(a.kind) * 7919 + a.state * 31 + SymbolHash()(a.symbol) * 17 + a.child_count +
                static_cast<size_t>(a.dynamic_precedence) * 13 + a.production_id * 101 + a.is_repetition;
        return h;
    }
};

class Generator {
public:
    std::string buffer;
    size_t indent_level = 0;
    std::string language_name;
    ParseTable parse_table;
    LexTable main_lex_table, keyword_lex_table;
    std::vector<std::pair<std::optional<Symbol>, CharacterSet>> large_character_sets;
    struct LargeCharacterSetInfo {
        std::string constant_name;
        bool is_used = false;
    };
    std::vector<LargeCharacterSetInfo> large_character_set_info;
    size_t large_state_count = 0;
    SyntaxGrammar syntax_grammar;
    LexicalGrammar lexical_grammar;
    AliasMap default_aliases;
    std::unordered_map<Symbol, size_t, SymbolHash> symbol_order;
    std::unordered_map<Symbol, std::string, SymbolHash> symbol_ids;
    std::unordered_map<Alias, std::string, AliasHash> alias_ids;
    std::vector<Alias> unique_aliases;
    std::unordered_map<Symbol, Symbol, SymbolHash> symbol_map;
    std::vector<TokenSet> reserved_word_sets;
    std::vector<size_t> reserved_word_set_ids_by_parse_state;
    std::vector<std::string> field_names;
    std::map<Symbol, std::vector<ChildType>> supertype_symbol_map;
    std::map<std::string, std::vector<ChildType>> supertype_map;
    size_t abi_version = 15;
    std::optional<SemanticVersion> metadata;

    std::string Generate() {
        Init();
        AddHeader();
        AddIncludes();
        AddPragmas();
        AddStats();
        AddSymbolEnum();
        AddSymbolNamesList();
        AddUniqueSymbolMap();
        AddSymbolMetadataList();
        if (!field_names.empty()) {
            AddFieldNameEnum();
            AddFieldNameNamesList();
            AddFieldSequences();
        }
        if (!parse_table.production_infos.empty()) AddAliasSequences();
        AddNonTerminalAliasMap();
        AddPrimaryStateIdList();
        if (abi_version >= kAbiWithReservedWords && !supertype_map.empty()) AddSupertypeMap();
        size_t before_lex = buffer.size();
        LexTable main = std::move(main_lex_table);
        AddLexFunction("ts_lex", std::move(main));
        if (syntax_grammar.word_token) AddLexFunction("ts_lex_keywords", std::move(keyword_lex_table));
        std::string lex_functions = buffer.substr(before_lex);
        buffer.resize(before_lex);
        for (size_t ix = 0; ix < large_character_sets.size(); ix++) AddCharacterSet(ix);
        buffer += lex_functions;
        AddLexModes();
        if (abi_version >= kAbiWithReservedWords && reserved_word_sets.size() > 1) AddReservedWordSets();
        AddParseTable();
        if (!syntax_grammar.external_tokens.empty()) {
            AddExternalTokenEnum();
            AddExternalScannerSymbolMap();
            AddExternalScannerStatesList();
        }
        AddParserExport();
        return std::move(buffer);
    }

private:
    void Whitespace() {
        for (size_t i = 0; i < indent_level; i++) buffer += "  ";
    }
    void Line(const std::string &s) {
        Whitespace();
        buffer += s;
        buffer += "\n";
    }
    void Add(const std::string &s) { buffer += s; }
    void Indent() { indent_level++; }
    void Dedent() { indent_level--; }

    std::pair<const std::string &, VariableType> MetadataForSymbol(const Symbol &s) const {
        static const std::string end = "end";
        switch (s.kind) {
            case SymbolType::End:
            case SymbolType::EndOfNonTerminalExtra:
                return {end, VariableType::Hidden};
            case SymbolType::NonTerminal: {
                const SyntaxVariable &v = syntax_grammar.variables[s.index];
                return {v.name, v.kind};
            }
            case SymbolType::Terminal: {
                const LexicalVariable &v = lexical_grammar.variables[s.index];
                return {v.name, v.kind};
            }
            case SymbolType::External: {
                const ExternalToken &t = syntax_grammar.external_tokens[s.index];
                return {t.name, t.kind};
            }
        }
        return {end, VariableType::Hidden};
    }

    const Alias *DefaultAlias(const Symbol &s) const {
        auto it = default_aliases.find(s);
        return it == default_aliases.end() ? nullptr : &it->second;
    }

    std::vector<Symbol> SymbolsForAlias(const Alias &alias) const {
        std::vector<Symbol> result;
        for (const Symbol &s : parse_table.symbols) {
            if (const Alias *d = DefaultAlias(s)) {
                if (*d == alias) result.push_back(s);
            } else {
                auto [name, kind] = MetadataForSymbol(s);
                if (name == alias.value && kind == alias.Kind()) result.push_back(s);
            }
        }
        return result;
    }

    void AssignSymbolId(const Symbol &symbol, std::unordered_set<std::string> &used) {
        std::string id;
        if (symbol == Symbol::End()) {
            id = "ts_builtin_sym_end";
        } else {
            auto [name, kind] = MetadataForSymbol(symbol);
            switch (kind) {
                case VariableType::Auxiliary: id = "aux_sym_" + SanitizeIdentifier(name); break;
                case VariableType::Anonymous: id = "anon_sym_" + SanitizeIdentifier(name); break;
                default: id = "sym_" + SanitizeIdentifier(name); break;
            }
            size_t suffix_number = 1;
            std::string suffix;
            while (used.count(id)) {
                id.resize(id.size() - suffix.size());
                suffix_number++;
                suffix = std::to_string(suffix_number);
                id += suffix;
            }
        }
        used.insert(id);
        symbol_ids[symbol] = id;
    }

    void Init() {
        std::unordered_set<std::string> used;
        for (const Symbol &s : parse_table.symbols) AssignSymbolId(s, used);
        symbol_ids[Symbol::EndOfNonTerminalExtra()] = symbol_ids.at(Symbol::End());
        for (const Symbol &symbol : parse_table.symbols) {
            Symbol mapping = symbol;
            if (const Alias *alias = DefaultAlias(symbol)) {
                VariableType kind = alias->Kind();
                for (const Symbol &other : parse_table.symbols) {
                    if (const Alias *other_alias = DefaultAlias(other)) {
                        if (other < mapping && *other_alias == *alias) mapping = other;
                    } else {
                        auto [name, k] = MetadataForSymbol(other);
                        if (name == alias->value && k == kind) {
                            mapping = other;
                            break;
                        }
                    }
                }
            } else if (symbol.IsTerminal()) {
                auto symbol_metadata = MetadataForSymbol(symbol);
                for (const Symbol &other : parse_table.symbols) {
                    auto other_metadata = MetadataForSymbol(other);
                    if (other_metadata.first == symbol_metadata.first && other_metadata.second == symbol_metadata.second) {
                        auto it = symbol_map.find(other);
                        if (it != symbol_map.end() && it->second == symbol) break;
                        mapping = other;
                        break;
                    }
                }
            }
            symbol_map[symbol] = mapping;
        }
        for (const ProductionInfo &info : parse_table.production_infos) {
            for (const auto &[field_name, locations] : info.field_map) {
                auto it = std::lower_bound(field_names.begin(), field_names.end(), field_name);
                if (it == field_names.end() || *it != field_name) field_names.insert(it, field_name);
            }
            for (const std::optional<Alias> &alias : info.alias_sequence) {
                if (!alias) continue;
                std::string alias_id;
                std::vector<Symbol> existing = SymbolsForAlias(*alias);
                if (!existing.empty()) {
                    alias_id = symbol_ids.at(symbol_map.at(existing.front()));
                } else {
                    auto it = std::lower_bound(unique_aliases.begin(), unique_aliases.end(), *alias);
                    if (it == unique_aliases.end() || !(*it == *alias)) unique_aliases.insert(it, *alias);
                    alias_id = (alias->is_named ? "alias_sym_" : "anon_alias_sym_") + SanitizeIdentifier(alias->value);
                }
                alias_ids.emplace(*alias, alias_id);
            }
        }
        for (size_t ix = 0; ix < large_character_sets.size(); ix++) {
            const auto &symbol = large_character_sets[ix].first;
            size_t count = 1;
            for (size_t k = 0; k < ix; k++)
                if (large_character_sets[k].first == symbol) count++;
            std::string name = symbol ? symbol_ids.at(*symbol) + "_character_set_" + std::to_string(count)
                                      : "extras_character_set_" + std::to_string(count);
            large_character_set_info.push_back({name, false});
        }
        reserved_word_sets.push_back(TokenSet{});
        for (const ParseState &state : parse_table.states) {
            size_t id = reserved_word_sets.size();
            for (size_t k = 0; k < reserved_word_sets.size(); k++)
                if (reserved_word_sets[k] == state.reserved_words) {
                    id = k;
                    break;
                }
            if (id == reserved_word_sets.size()) reserved_word_sets.push_back(state.reserved_words);
            reserved_word_set_ids_by_parse_state.push_back(id);
        }
        if (abi_version >= kAbiWithReservedWords) {
            for (const auto &[supertype, subtypes] : supertype_symbol_map) {
                auto it = symbol_ids.find(supertype);
                if (it != symbol_ids.end()) supertype_map.emplace(it->second, subtypes);
            }
            supertype_symbol_map.clear();
        }
        size_t threshold = std::min(kSmallStateThreshold, parse_table.symbols.size() / 2);
        large_state_count = 0;
        for (size_t i = 0; i < parse_table.states.size(); i++) {
            const ParseState &s = parse_table.states[i];
            if (i <= 1 || s.terminal_entries.size() + s.nonterminal_entries.size() > threshold) large_state_count++;
            else break;
        }
    }

    void AddHeader() {
        Line(std::string("/* Automatically @generated by tree-sitter v") + kBuildVersion + " */");
        Line("");
    }

    void AddIncludes() {
        Line("#include \"tree_sitter/parser.h\"");
        Line("");
    }

    void AddPragmas() {
        Line("#if defined(__GNUC__) || defined(__clang__)");
        Line("#pragma GCC diagnostic ignored \"-Wmissing-field-initializers\"");
        Line("#endif");
        Line("");
        if (main_lex_table.states.size() > 300) {
            Line("#ifdef _MSC_VER");
            Line("#pragma optimize(\"\", off)");
            Line("#elif defined(__clang__)");
            Line("#pragma clang optimize off");
            Line("#elif defined(__GNUC__)");
            Line("#pragma GCC optimize (\"O0\")");
            Line("#endif");
            Line("");
        }
    }

    size_t MaxReservedWordSetSize() const {
        size_t m = 0;
        for (const TokenSet &t : reserved_word_sets) m = std::max(m, t.Len());
        return m;
    }

    void AddStats() {
        size_t token_count = 0;
        for (const Symbol &s : parse_table.symbols) {
            if (s.IsTerminal() || s.IsEof()) token_count++;
            else if (s.IsExternal() && !syntax_grammar.external_tokens[s.index].corresponding_internal_token) token_count++;
        }
        Line("#define LANGUAGE_VERSION " + std::to_string(abi_version));
        Line("#define STATE_COUNT " + std::to_string(parse_table.states.size()));
        Line("#define LARGE_STATE_COUNT " + std::to_string(large_state_count));
        Line("#define SYMBOL_COUNT " + std::to_string(parse_table.symbols.size()));
        Line("#define ALIAS_COUNT " + std::to_string(unique_aliases.size()));
        Line("#define TOKEN_COUNT " + std::to_string(token_count));
        Line("#define EXTERNAL_TOKEN_COUNT " + std::to_string(syntax_grammar.external_tokens.size()));
        Line("#define FIELD_COUNT " + std::to_string(field_names.size()));
        Line("#define MAX_ALIAS_SEQUENCE_LENGTH " + std::to_string(parse_table.max_aliased_production_length));
        Line("#define MAX_RESERVED_WORD_SET_SIZE " + std::to_string(MaxReservedWordSetSize()));
        Line("#define PRODUCTION_ID_COUNT " + std::to_string(parse_table.production_infos.size()));
        Line("#define SUPERTYPE_COUNT " + std::to_string(supertype_map.size()));
        Line("");
    }

    void AddSymbolEnum() {
        Line("enum ts_symbol_identifiers {");
        Indent();
        symbol_order[Symbol::End()] = 0;
        size_t i = 1;
        for (const Symbol &s : parse_table.symbols) {
            if (s == Symbol::End()) continue;
            symbol_order[s] = i;
            Line(symbol_ids.at(s) + " = " + std::to_string(i) + ",");
            i++;
        }
        for (const Alias &a : unique_aliases) {
            Line(alias_ids.at(a) + " = " + std::to_string(i) + ",");
            i++;
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddSymbolNamesList() {
        Line("static const char * const ts_symbol_names[] = {");
        Indent();
        for (const Symbol &s : parse_table.symbols) {
            const Alias *a = DefaultAlias(s);
            std::string name = SanitizeString(a ? a->value : MetadataForSymbol(s).first);
            Line("[" + symbol_ids.at(s) + "] = \"" + name + "\",");
        }
        for (const Alias &a : unique_aliases) Line("[" + alias_ids.at(a) + "] = \"" + SanitizeString(a.value) + "\",");
        Dedent();
        Line("};");
        Line("");
    }

    void AddUniqueSymbolMap() {
        Line("static const TSSymbol ts_symbol_map[] = {");
        Indent();
        for (const Symbol &s : parse_table.symbols) Line("[" + symbol_ids.at(s) + "] = " + symbol_ids.at(symbol_map.at(s)) + ",");
        for (const Alias &a : unique_aliases) Line("[" + alias_ids.at(a) + "] = " + alias_ids.at(a) + ",");
        Dedent();
        Line("};");
        Line("");
    }

    static std::string FieldId(const std::string &f) { return "field_" + f; }

    void AddFieldNameEnum() {
        Line("enum ts_field_identifiers {");
        Indent();
        for (size_t i = 0; i < field_names.size(); i++) Line(FieldId(field_names[i]) + " = " + std::to_string(i + 1) + ",");
        Dedent();
        Line("};");
        Line("");
    }

    void AddFieldNameNamesList() {
        Line("static const char * const ts_field_names[] = {");
        Indent();
        Line("[0] = NULL,");
        for (const std::string &f : field_names) Line("[" + FieldId(f) + "] = \"" + f + "\",");
        Dedent();
        Line("};");
        Line("");
    }

    void AddSymbolMetadataList() {
        Line("static const TSSymbolMetadata ts_symbol_metadata[] = {");
        Indent();
        for (const Symbol &s : parse_table.symbols) {
            Line("[" + symbol_ids.at(s) + "] = {");
            Indent();
            if (const Alias *a = DefaultAlias(s)) {
                Line(".visible = true,");
                Line(".named = " + Bool(a->is_named) + ",");
            } else {
                switch (MetadataForSymbol(s).second) {
                    case VariableType::Named:
                        Line(".visible = true,");
                        Line(".named = true,");
                        break;
                    case VariableType::Anonymous:
                        Line(".visible = true,");
                        Line(".named = false,");
                        break;
                    case VariableType::Hidden:
                        Line(".visible = false,");
                        Line(".named = true,");
                        if (Contains(syntax_grammar.supertype_symbols, s)) Line(".supertype = true,");
                        break;
                    case VariableType::Auxiliary:
                        Line(".visible = false,");
                        Line(".named = false,");
                        break;
                }
            }
            Dedent();
            Line("},");
        }
        for (const Alias &a : unique_aliases) {
            Line("[" + alias_ids.at(a) + "] = {");
            Indent();
            Line(".visible = true,");
            Line(".named = " + Bool(a.is_named) + ",");
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddAliasSequences() {
        Line("static const TSSymbol ts_alias_sequences[PRODUCTION_ID_COUNT][MAX_ALIAS_SEQUENCE_LENGTH] = {");
        Indent();
        for (size_t i = 0; i < parse_table.production_infos.size(); i++) {
            const ProductionInfo &info = parse_table.production_infos[i];
            if (info.alias_sequence.empty()) {
                if (i == 0) Line("[0] = {0},");
                continue;
            }
            Line("[" + std::to_string(i) + "] = {");
            Indent();
            for (size_t j = 0; j < info.alias_sequence.size(); j++)
                if (info.alias_sequence[j]) Line("[" + std::to_string(j) + "] = " + alias_ids.at(*info.alias_sequence[j]) + ",");
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddNonTerminalAliasMap() {
        std::map<Symbol, std::vector<std::string>> alias_ids_by_symbol;
        for (const SyntaxVariable &v : syntax_grammar.variables) {
            for (const Production &p : v.productions) {
                for (const ProductionStep &step : p.steps) {
                    if (!step.alias || !step.symbol.IsNonTerminal()) continue;
                    const Alias *d = DefaultAlias(step.symbol);
                    if (d && *d == *step.alias) continue;
                    if (!symbol_ids.count(step.symbol)) continue;
                    auto it = alias_ids.find(*step.alias);
                    if (it == alias_ids.end()) continue;
                    std::vector<std::string> &ids = alias_ids_by_symbol[step.symbol];
                    auto pos = std::lower_bound(ids.begin(), ids.end(), it->second);
                    if (pos == ids.end() || *pos != it->second) ids.insert(pos, it->second);
                }
            }
        }
        Line("static const uint16_t ts_non_terminal_alias_map[] = {");
        Indent();
        for (const auto &[symbol, ids] : alias_ids_by_symbol) {
            Line(symbol_ids.at(symbol) + ", " + std::to_string(1 + ids.size()) + ",");
            Indent();
            Line(symbol_ids.at(symbol_map.at(symbol)) + ",");
            for (const std::string &id : ids) Line(id + ",");
            Dedent();
        }
        Line("0,");
        Dedent();
        Line("};");
        Line("");
    }

    void AddPrimaryStateIdList() {
        Line("static const TSStateId ts_primary_state_ids[STATE_COUNT] = {");
        Indent();
        std::unordered_map<size_t, size_t> first;
        for (size_t idx = 0; idx < parse_table.states.size(); idx++) {
            size_t primary = first.emplace(parse_table.states[idx].core_id, idx).first->second;
            Line("[" + std::to_string(idx) + "] = " + std::to_string(primary) + ",");
        }
        Dedent();
        Line("};");
        Line("");
    }

    using FlatFieldMap = std::vector<std::pair<std::string, FieldLocation>>;

    size_t GetFieldMapId(FlatFieldMap flat, std::vector<std::pair<size_t, FlatFieldMap>> &maps, size_t &next) {
        for (const auto &[index, e] : maps)
            if (e == flat) return index;
        size_t result = next;
        next += flat.size();
        maps.push_back({result, std::move(flat)});
        return result;
    }

    void AddFieldSequences() {
        std::vector<std::pair<size_t, FlatFieldMap>> flat_field_maps;
        size_t next = 0;
        GetFieldMapId({}, flat_field_maps, next);
        std::vector<std::pair<size_t, size_t>> field_map_ids;
        for (const ProductionInfo &info : parse_table.production_infos) {
            if (info.field_map.empty()) {
                field_map_ids.push_back({0, 0});
                continue;
            }
            FlatFieldMap flat;
            for (const auto &[name, locations] : info.field_map)
                for (const FieldLocation &l : locations) flat.push_back({name, l});
            size_t len = flat.size();
            field_map_ids.push_back({GetFieldMapId(flat, flat_field_maps, next), len});
        }
        Line("static const TSMapSlice ts_field_map_slices[PRODUCTION_ID_COUNT] = {");
        Indent();
        for (size_t pid = 0; pid < field_map_ids.size(); pid++) {
            auto [row_id, length] = field_map_ids[pid];
            if (length > 0)
                Line("[" + std::to_string(pid) + "] = {.index = " + std::to_string(row_id) + ", .length = " + std::to_string(length) + "},");
        }
        Dedent();
        Line("};");
        Line("");
        Line("static const TSFieldMapEntry ts_field_map_entries[] = {");
        Indent();
        for (size_t k = 1; k < flat_field_maps.size(); k++) {
            const auto &[row_index, pairs] = flat_field_maps[k];
            Line("[" + std::to_string(row_index) + "] =");
            Indent();
            for (const auto &[field_name, location] : pairs) {
                Whitespace();
                Add("{" + FieldId(field_name) + ", " + std::to_string(location.index));
                if (location.inherited) Add(", .inherited = true");
                Add("},\n");
            }
            Dedent();
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddSupertypeMap() {
        Line("static const TSSymbol ts_supertype_symbols[SUPERTYPE_COUNT] = {");
        Indent();
        for (const auto &[supertype, subtypes] : supertype_map) Line(supertype + ",");
        Dedent();
        Line("};\n");
        Line("static const TSMapSlice ts_supertype_map_slices[] = {");
        Indent();
        size_t row_id = 0;
        std::vector<size_t> supertype_ids{0};
        std::map<std::string, std::set<std::string>> string_map;
        for (const auto &[supertype, subtypes] : supertype_map) {
            std::set<std::string> ids;
            for (const ChildType &t : subtypes) {
                if (!t.aliased) {
                    auto it = symbol_ids.find(t.symbol);
                    if (it != symbol_ids.end()) ids.insert(it->second);
                } else {
                    auto it = alias_ids.find(t.alias);
                    if (it != alias_ids.end()) {
                        ids.insert(it->second);
                    } else {
                        for (const Symbol &s : SymbolsForAlias(t.alias)) {
                            auto sit = symbol_ids.find(s);
                            if (sit != symbol_ids.end()) ids.insert(sit->second);
                        }
                    }
                }
            }
            string_map[supertype] = std::move(ids);
        }
        for (const auto &[supertype, subtypes] : string_map) {
            size_t length = subtypes.size();
            Line("[" + supertype + "] = {.index = " + std::to_string(row_id) + ", .length = " + std::to_string(length) + "},");
            row_id += length;
            supertype_ids.push_back(row_id);
        }
        Dedent();
        Line("};");
        Line("");
        Line("static const TSSymbol ts_supertype_map_entries[] = {");
        Indent();
        size_t i = 0;
        for (const auto &[supertype, subtypes] : string_map) {
            Line("[" + std::to_string(supertype_ids[i]) + "] =");
            Indent();
            for (const std::string &s : subtypes) {
                Whitespace();
                Add(s + ",\n");
            }
            Dedent();
            i++;
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddLexFunction(const std::string &name, LexTable table) {
        Line("static bool " + name + "(TSLexer *lexer, TSStateId state) {");
        Indent();
        Line("START_LEXER();");
        Line("eof = lexer->eof(lexer);");
        Line("switch (state) {");
        Indent();
        for (size_t i = 0; i < table.states.size(); i++) {
            Line("case " + std::to_string(i) + ":");
            Indent();
            AddLexState(std::move(table.states[i]));
            Dedent();
        }
        Line("default:");
        Indent();
        Line("return false;");
        Dedent();
        Dedent();
        Line("}");
        Dedent();
        Line("}");
        Line("");
    }

    void AddCharacter(uint32_t c) {
        switch (c) {
            case '\'': Add("'\\''"); return;
            case '\\': Add("'\\\\'"); return;
            case 0x0c: Add("'\\f'"); return;
            case '\n': Add("'\\n'"); return;
            case '\t': Add("'\\t'"); return;
            case '\r': Add("'\\r'"); return;
            default: break;
        }
        if (c == 0) Add("0");
        else if (c == ' ' || (c >= '!' && c <= '~')) Add(std::string("'") + static_cast<char>(c) + "'");
        else Add("0x" + Hex(c, 2));
    }

    void AddAdvanceAction(const AdvanceAction &a) {
        Add((a.in_main_token ? "ADVANCE(" : "SKIP(") + std::to_string(a.state) + ");");
    }

    void AddLexState(LexState state) {
        if (state.accept_action) Line("ACCEPT_TOKEN(" + symbol_ids.at(*state.accept_action) + ");");
        if (state.eof_action) Line("if (eof) ADVANCE(" + std::to_string(state.eof_action->state) + ");");
        CharacterSet ruled_out_chars;
        size_t leading_count = 0, leading_range_count = 0;
        for (const auto &[chars, action] : state.advance_actions) {
            bool simple = action.in_main_token;
            if (simple)
                for (const auto &r : chars.CharRanges())
                    if (!(r.second <= r.first + 1 && r.second <= 0xFFFF)) simple = false;
            if (!simple) break;
            leading_count++;
            leading_range_count += chars.RangeCount();
        }
        if (leading_range_count >= 8) {
            Line("ADVANCE_MAP(");
            Indent();
            for (size_t k = 0; k < leading_count; k++) {
                const auto &[chars, action] = state.advance_actions[k];
                for (const auto &r : chars.CharRanges()) {
                    Whitespace();
                    AddCharacter(r.first);
                    Add(", " + std::to_string(action.state) + ",\n");
                    if (r.second > r.first) {
                        Whitespace();
                        AddCharacter(r.second);
                        Add(", " + std::to_string(action.state) + ",\n");
                    }
                }
                ruled_out_chars = ruled_out_chars.Add(chars);
            }
            Dedent();
            Line(");");
        } else {
            leading_count = 0;
        }
        for (size_t k = leading_count; k < state.advance_actions.size(); k++) {
            const auto &[chars, action] = state.advance_actions[k];
            Whitespace();
            CharacterSet simplified = chars.SimplifyIgnoring(ruled_out_chars);
            struct Best {
                size_t ix;
                CharacterSet additions, removals;
            };
            std::optional<Best> best;
            if (simplified.RangeCount() >= kLargeCharacterRangeCount) {
                for (size_t ix = 0; ix < large_character_sets.size(); ix++) {
                    CharacterSet chars_copy = simplified;
                    CharacterSet large_set = large_character_sets[ix].second;
                    CharacterSet intersection = chars_copy.RemoveIntersection(large_set);
                    if (intersection.IsEmpty()) continue;
                    CharacterSet additions = chars_copy.SimplifyIgnoring(ruled_out_chars);
                    CharacterSet removals = large_set.SimplifyIgnoring(ruled_out_chars);
                    size_t total = additions.RangeCount() + removals.RangeCount();
                    if (total >= simplified.RangeCount()) continue;
                    if (best && best->additions.RangeCount() + best->removals.RangeCount() < total) continue;
                    best = Best{ix, std::move(additions), std::move(removals)};
                }
            }
            ruled_out_chars = ruled_out_chars.Add(chars);
            std::optional<size_t> large_ix;
            CharacterSet asserted = std::move(simplified);
            CharacterSet negated;
            if (best) {
                asserted = std::move(best->additions);
                negated = std::move(best->removals);
                large_ix = best->ix;
            }
            std::string line_break = "\n";
            for (size_t n = 0; n < indent_level + 2; n++) line_break += "  ";
            bool has_positive = large_ix.has_value() || !asserted.IsEmpty();
            bool has_negative = !negated.IsEmpty();
            bool has_condition = has_positive || has_negative;
            if (has_condition) {
                Add("if (");
                if (has_positive && has_negative) Add("(");
            }
            if (large_ix) {
                const CharacterSet &large_set = large_character_sets[*large_ix].second;
                bool check_eof = large_set.Contains(0);
                if (check_eof) Add("(!eof && ");
                large_character_set_info[*large_ix].is_used = true;
                Add("set_contains(" + large_character_set_info[*large_ix].constant_name + ", " +
                    std::to_string(large_set.RangeCount()) + ", lookahead)");
                if (check_eof) Add(")");
            }
            if (!asserted.IsEmpty()) {
                if (large_ix) Add(" ||" + line_break);
                bool is_included = !asserted.Contains(0x10FFFF);
                if (!is_included) asserted = asserted.Negate().AddChar(0);
                AddCharacterRangeConditions(asserted, is_included, line_break);
            }
            if (has_negative) {
                if (has_positive) Add(") &&" + line_break);
                AddCharacterRangeConditions(negated, false, line_break);
            }
            if (has_condition) Add(") ");
            AddAdvanceAction(action);
            Add("\n");
        }
        Line("END_STATE();");
    }

    void AddCharacterRangeConditions(const CharacterSet &characters, bool is_included, const std::string &line_break) {
        std::vector<std::pair<uint32_t, uint32_t>> ranges = characters.CharRanges();
        for (size_t i = 0; i < ranges.size(); i++) {
            uint32_t start = ranges[i].first, end = ranges[i].second;
            if (is_included) {
                if (i > 0) Add(" ||" + line_break);
                if (start == 0) {
                    Add("(!eof && ");
                    Add(end == 0 ? "lookahead == 0" : "lookahead <= ");
                    AddCharacter(end);
                    Add(")");
                } else if (end == start) {
                    Add("lookahead == ");
                    AddCharacter(start);
                } else if (end == start + 1) {
                    Add("lookahead == ");
                    AddCharacter(start);
                    Add(" ||" + line_break + "lookahead == ");
                    AddCharacter(end);
                } else {
                    Add("(");
                    AddCharacter(start);
                    Add(" <= lookahead && lookahead <= ");
                    AddCharacter(end);
                    Add(")");
                }
            } else {
                if (i > 0) Add(" &&" + line_break);
                if (end == start) {
                    Add("lookahead != ");
                    AddCharacter(start);
                } else if (end == start + 1) {
                    Add("lookahead != ");
                    AddCharacter(start);
                    Add(" &&" + line_break + "lookahead != ");
                    AddCharacter(end);
                } else if (start != 0) {
                    Add("(lookahead < ");
                    AddCharacter(start);
                    Add(" || ");
                    AddCharacter(end);
                    Add(" < lookahead)");
                } else {
                    Add("lookahead > ");
                    AddCharacter(end);
                }
            }
        }
    }

    void AddCharacterSet(size_t ix) {
        if (!large_character_set_info[ix].is_used) return;
        CharacterSet characters = large_character_sets[ix].second;
        Line("static const TSCharacterRange " + large_character_set_info[ix].constant_name + "[] = {");
        Indent();
        std::vector<std::pair<uint32_t, uint32_t>> ranges = characters.CharRanges();
        for (size_t k = 0; k < ranges.size(); k++) {
            if (k % 8 == 0) {
                if (k > 0) Add("\n");
                Whitespace();
            } else {
                Add(" ");
            }
            Add("{");
            AddCharacter(ranges[k].first);
            Add(", ");
            AddCharacter(ranges[k].second);
            Add("},");
        }
        Add("\n");
        Dedent();
        Line("};");
        Line("");
    }

    void AddLexModes() {
        Line(std::string("static const ") + (abi_version >= kAbiWithReservedWords ? "TSLexerMode" : "TSLexMode") +
             " ts_lex_modes[STATE_COUNT] = {");
        Indent();
        for (size_t i = 0; i < parse_table.states.size(); i++) {
            const ParseState &state = parse_table.states[i];
            Whitespace();
            Add("[" + std::to_string(i) + "] = {");
            if (state.IsEndOfNonTerminalExtra()) {
                Add("(TSStateId)(-1),");
            } else {
                Add(".lex_state = " + std::to_string(state.lex_state_id));
                if (state.external_lex_state_id > 0) Add(", .external_lex_state = " + std::to_string(state.external_lex_state_id));
                if (abi_version >= kAbiWithReservedWords) {
                    size_t id = reserved_word_set_ids_by_parse_state[i];
                    if (id != 0) Add(", .reserved_word_set_id = " + std::to_string(id));
                }
            }
            Add("},\n");
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddReservedWordSets() {
        Line("static const TSSymbol ts_reserved_words[" + std::to_string(reserved_word_sets.size()) + "][MAX_RESERVED_WORD_SET_SIZE] = {");
        Indent();
        for (size_t id = 1; id < reserved_word_sets.size(); id++) {
            Line("[" + std::to_string(id) + "] = {");
            Indent();
            for (const Symbol &t : reserved_word_sets[id].Symbols()) Line(symbol_ids.at(t) + ",");
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("");
    }

    std::string ExternalTokenId(const ExternalToken &t) const { return "ts_external_token_" + SanitizeIdentifier(t.name); }

    void AddExternalTokenEnum() {
        Line("enum ts_external_scanner_symbol_identifiers {");
        Indent();
        for (size_t i = 0; i < syntax_grammar.external_tokens.size(); i++)
            Line(ExternalTokenId(syntax_grammar.external_tokens[i]) + " = " + std::to_string(i) + ",");
        Dedent();
        Line("};");
        Line("");
    }

    void AddExternalScannerSymbolMap() {
        Line("static const TSSymbol ts_external_scanner_symbol_map[EXTERNAL_TOKEN_COUNT] = {");
        Indent();
        for (size_t i = 0; i < syntax_grammar.external_tokens.size(); i++) {
            const ExternalToken &t = syntax_grammar.external_tokens[i];
            Symbol id_token = t.corresponding_internal_token ? *t.corresponding_internal_token : Symbol::External(i);
            Line("[" + ExternalTokenId(t) + "] = " + symbol_ids.at(id_token) + ",");
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddExternalScannerStatesList() {
        Line("static const bool ts_external_scanner_states[" + std::to_string(parse_table.external_lex_states.size()) +
             "][EXTERNAL_TOKEN_COUNT] = {");
        Indent();
        for (size_t i = 0; i < parse_table.external_lex_states.size(); i++) {
            const TokenSet &set = parse_table.external_lex_states[i];
            if (set.IsEmpty()) continue;
            Line("[" + std::to_string(i) + "] = {");
            Indent();
            for (const Symbol &t : set.Symbols()) Line("[" + ExternalTokenId(syntax_grammar.external_tokens[t.index]) + "] = true,");
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("");
    }

    size_t GetParseActionListId(const ParseTableEntry &entry, std::unordered_map<ParseTableEntry, size_t, EntryHash> &entries,
                                size_t &next) {
        auto it = entries.find(entry);
        if (it != entries.end()) return it->second;
        size_t result = next;
        entries.emplace(entry, result);
        next += 1 + entry.actions.size();
        return result;
    }

    std::vector<std::pair<Symbol, const ParseTableEntry *>> SortedTerminalEntries(const ParseState &state) {
        std::vector<std::pair<Symbol, const ParseTableEntry *>> v;
        for (const auto &[s, e] : state.terminal_entries.Entries()) v.push_back({s, &e});
        // sort_unstable_by_key(symbol_order.get(symbol)): an Option, None first; the keys are distinct.
        auto key = [&](const Symbol &s) -> std::pair<int, size_t> {
            auto it = symbol_order.find(s);
            return it == symbol_order.end() ? std::pair<int, size_t>{0, 0} : std::pair<int, size_t>{1, it->second};
        };
        std::sort(v.begin(), v.end(), [&](const auto &a, const auto &b) { return key(a.first) < key(b.first); });
        return v;
    }

    void AddParseTable() {
        std::unordered_map<ParseTableEntry, size_t, EntryHash> entries;
        size_t next = 0;
        GetParseActionListId(ParseTableEntry{{}, false}, entries, next);
        Line("static const uint16_t ts_parse_table[LARGE_STATE_COUNT][SYMBOL_COUNT] = {");
        Indent();
        for (size_t i = 0; i < large_state_count; i++) {
            const ParseState &state = parse_table.states[i];
            Line("[STATE(" + std::to_string(i) + ")] = {");
            Indent();
            std::vector<std::pair<Symbol, GotoAction>> nonterminal(state.nonterminal_entries.Entries().begin(),
                                                                   state.nonterminal_entries.Entries().end());
            std::sort(nonterminal.begin(), nonterminal.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            for (const auto &[symbol, action] : nonterminal)
                Line("[" + symbol_ids.at(symbol) + "] = STATE(" + std::to_string(action.shift_extra ? i : action.state) + "),");
            for (const auto &[symbol, entry] : SortedTerminalEntries(state)) {
                size_t entry_id = GetParseActionListId(*entry, entries, next);
                Line("[" + symbol_ids.at(symbol) + "] = ACTIONS(" + std::to_string(entry_id) + "),");
            }
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("");
        if (large_state_count < parse_table.states.size()) {
            Line("static const uint16_t ts_small_parse_table[] = {");
            Indent();
            size_t next_table_index = 0;
            std::vector<size_t> small_state_indices;
            for (size_t si = large_state_count; si < parse_table.states.size(); si++) {
                const ParseState &state = parse_table.states[si];
                small_state_indices.push_back(next_table_index);
                // (value, kind) -> symbols, in first-insertion order of keys irrelevant: sorted below.
                std::map<std::pair<size_t, SymbolType>, std::vector<Symbol>> symbols_by_value;
                for (const auto &[symbol, entry] : SortedTerminalEntries(state)) {
                    size_t entry_id = GetParseActionListId(*entry, entries, next);
                    symbols_by_value[{entry_id, SymbolType::Terminal}].push_back(symbol);
                }
                for (const auto &[symbol, action] : state.nonterminal_entries.Entries()) {
                    size_t state_id = action.shift_extra ? large_state_count + small_state_indices.size() - 1 : action.state;
                    symbols_by_value[{state_id, SymbolType::NonTerminal}].push_back(symbol);
                }
                std::vector<std::pair<std::pair<size_t, SymbolType>, std::vector<Symbol>>> values(symbols_by_value.begin(),
                                                                                                   symbols_by_value.end());
                std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) {
                    auto ka = std::make_tuple(a.second.size(), a.first.second, a.first.first, a.second[0]);
                    auto kb = std::make_tuple(b.second.size(), b.first.second, b.first.first, b.second[0]);
                    return ka < kb;
                });
                Line("[" + std::to_string(next_table_index) + "] = " + std::to_string(values.size()) + ",");
                Indent();
                next_table_index++;
                for (auto &[key, symbols] : values) {
                    next_table_index += 2 + symbols.size();
                    if (key.second == SymbolType::NonTerminal)
                        Line("STATE(" + std::to_string(key.first) + "), " + std::to_string(symbols.size()) + ",");
                    else
                        Line("ACTIONS(" + std::to_string(key.first) + "), " + std::to_string(symbols.size()) + ",");
                    std::sort(symbols.begin(), symbols.end());
                    Indent();
                    for (const Symbol &s : symbols) Line(symbol_ids.at(s) + ",");
                    Dedent();
                }
                Dedent();
            }
            Dedent();
            Line("};");
            Line("");
            Line("static const uint32_t ts_small_parse_table_map[] = {");
            Indent();
            for (size_t i = large_state_count; i < parse_table.states.size(); i++)
                Line("[SMALL_STATE(" + std::to_string(i) + ")] = " + std::to_string(small_state_indices[i - large_state_count]) + ",");
            Dedent();
            Line("};");
            Line("");
        }
        std::vector<std::pair<size_t, ParseTableEntry>> list;
        for (auto &[entry, i] : entries) list.push_back({i, entry});
        std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        AddParseActionList(list);
    }

    void AddParseActionList(const std::vector<std::pair<size_t, ParseTableEntry>> &list) {
        Line("static const TSParseActionEntry ts_parse_actions[] = {");
        Indent();
        for (const auto &[i, entry] : list) {
            Add("  [" + std::to_string(i) + "] = {.entry = {.count = " + std::to_string(entry.actions.size()) +
                ", .reusable = " + Bool(entry.reusable) + "}},");
            for (const ParseAction &a : entry.actions) {
                Add(" ");
                switch (a.kind) {
                    case ParseAction::Kind::Accept: Add(" ACCEPT_INPUT()"); break;
                    case ParseAction::Kind::Recover: Add("RECOVER()"); break;
                    case ParseAction::Kind::ShiftExtra: Add("SHIFT_EXTRA()"); break;
                    case ParseAction::Kind::Shift:
                        Add((a.is_repetition ? "SHIFT_REPEAT(" : "SHIFT(") + std::to_string(a.state) + ")");
                        break;
                    case ParseAction::Kind::Reduce:
                        Add("REDUCE(" + symbol_ids.at(a.symbol) + ", " + std::to_string(a.child_count) + ", " +
                            std::to_string(a.dynamic_precedence) + ", " + std::to_string(a.production_id) + ")");
                        break;
                }
                Add(",");
            }
            Add("\n");
        }
        Dedent();
        Line("};");
        Line("");
    }

    void AddParserExport() {
        std::string fn = "tree_sitter_" + language_name;
        std::string scanner = fn + "_external_scanner";
        Line("#ifdef __cplusplus");
        Line("extern \"C\" {");
        Line("#endif");
        if (!syntax_grammar.external_tokens.empty()) {
            Line("void *" + scanner + "_create(void);");
            Line("void " + scanner + "_destroy(void *);");
            Line("bool " + scanner + "_scan(void *, TSLexer *, const bool *);");
            Line("unsigned " + scanner + "_serialize(void *, char *);");
            Line("void " + scanner + "_deserialize(void *, const char *, unsigned);");
            Line("");
        }
        Line("#ifdef TREE_SITTER_HIDE_SYMBOLS");
        Line("#define TS_PUBLIC");
        Line("#elif defined(_WIN32)");
        Line("#define TS_PUBLIC __declspec(dllexport)");
        Line("#else");
        Line("#define TS_PUBLIC __attribute__((visibility(\"default\")))");
        Line("#endif");
        Line("");
        Line("TS_PUBLIC const TSLanguage *" + fn + "(void) {");
        Indent();
        Line("static const TSLanguage language = {");
        Indent();
        Line(".abi_version = LANGUAGE_VERSION,");
        Line(".symbol_count = SYMBOL_COUNT,");
        Line(".alias_count = ALIAS_COUNT,");
        Line(".token_count = TOKEN_COUNT,");
        Line(".external_token_count = EXTERNAL_TOKEN_COUNT,");
        Line(".state_count = STATE_COUNT,");
        Line(".large_state_count = LARGE_STATE_COUNT,");
        Line(".production_id_count = PRODUCTION_ID_COUNT,");
        if (abi_version >= kAbiWithReservedWords) Line(".supertype_count = SUPERTYPE_COUNT,");
        Line(".field_count = FIELD_COUNT,");
        Line(".max_alias_sequence_length = MAX_ALIAS_SEQUENCE_LENGTH,");
        Line(".parse_table = &ts_parse_table[0][0],");
        if (large_state_count < parse_table.states.size()) {
            Line(".small_parse_table = ts_small_parse_table,");
            Line(".small_parse_table_map = ts_small_parse_table_map,");
        }
        Line(".parse_actions = ts_parse_actions,");
        Line(".symbol_names = ts_symbol_names,");
        if (!field_names.empty()) {
            Line(".field_names = ts_field_names,");
            Line(".field_map_slices = ts_field_map_slices,");
            Line(".field_map_entries = ts_field_map_entries,");
        }
        if (!supertype_map.empty() && abi_version >= kAbiWithReservedWords) {
            Line(".supertype_map_slices = ts_supertype_map_slices,");
            Line(".supertype_map_entries = ts_supertype_map_entries,");
            Line(".supertype_symbols = ts_supertype_symbols,");
        }
        Line(".symbol_metadata = ts_symbol_metadata,");
        Line(".public_symbol_map = ts_symbol_map,");
        Line(".alias_map = ts_non_terminal_alias_map,");
        if (!parse_table.production_infos.empty()) Line(".alias_sequences = &ts_alias_sequences[0][0],");
        Line(".lex_modes = (const void*)ts_lex_modes,");
        Line(".lex_fn = ts_lex,");
        if (syntax_grammar.word_token) {
            Line(".keyword_lex_fn = ts_lex_keywords,");
            Line(".keyword_capture_token = " + symbol_ids.at(*syntax_grammar.word_token) + ",");
        }
        if (!syntax_grammar.external_tokens.empty()) {
            Line(".external_scanner = {");
            Indent();
            Line("&ts_external_scanner_states[0][0],");
            Line("ts_external_scanner_symbol_map,");
            Line(scanner + "_create,");
            Line(scanner + "_destroy,");
            Line(scanner + "_scan,");
            Line(scanner + "_serialize,");
            Line(scanner + "_deserialize,");
            Dedent();
            Line("},");
        }
        Line(".primary_state_ids = ts_primary_state_ids,");
        if (abi_version >= kAbiWithReservedWords) {
            Line(".name = \"" + language_name + "\",");
            if (reserved_word_sets.size() > 1) Line(".reserved_words = &ts_reserved_words[0][0],");
            Line(".max_reserved_word_set_size = " + std::to_string(MaxReservedWordSetSize()) + ",");
            if (!metadata)
                Fail("Metadata is required to generate ABI version " + std::to_string(abi_version) +
                     ".\nThis means that your grammar doesn't have a tree-sitter.json config file with an appropriate "
                     "version field in the metadata table.");
            Line(".metadata = {");
            Indent();
            Line(".major_version = " + std::to_string(metadata->major) + ",");
            Line(".minor_version = " + std::to_string(metadata->minor) + ",");
            Line(".patch_version = " + std::to_string(metadata->patch) + ",");
            Dedent();
            Line("},");
        }
        Dedent();
        Line("};");
        Line("return &language;");
        Dedent();
        Line("}");
        Line("#ifdef __cplusplus");
        Line("}");
        Line("#endif");
    }
};

}  // namespace

std::string RenderC(const std::string &name, Tables tables, SyntaxGrammar syntax, LexicalGrammar lexical,
                    AliasMap default_aliases, size_t abi_version, std::optional<SemanticVersion> semantic_version,
                    std::map<Symbol, std::vector<ChildType>> supertype_symbol_map) {
    if (abi_version < 14 || abi_version > 15)
        Fail("This version of Tree-sitter can only generate parsers with ABI version 14 - 15, not " + std::to_string(abi_version));
    Generator g;
    g.language_name = name;
    g.parse_table = std::move(tables.parse_table);
    g.main_lex_table = std::move(tables.main_lex_table);
    g.keyword_lex_table = std::move(tables.keyword_lex_table);
    g.large_character_sets = std::move(tables.large_character_sets);
    g.syntax_grammar = std::move(syntax);
    g.lexical_grammar = std::move(lexical);
    g.default_aliases = std::move(default_aliases);
    g.abi_version = abi_version;
    g.metadata = semantic_version;
    g.supertype_symbol_map = std::move(supertype_symbol_map);
    return g.Generate();
}

}  // namespace tsgen
