// mep's OCaml language server: modules. Indexes a parsed .ml / .mli into its
// items (with their odoc comments attached the way odoc attaches them), finds
// compilation units on the load path, and resolves module paths through
// aliases, module types and includes. See ocaml_lsp_internal.h.

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "ocaml_lsp_internal.h"

namespace mllsp {

// --- tree-sitter helpers -------------------------------------------------------------

Tree::~Tree() {
    if (tree) ts_tree_delete(tree);
}

std::unique_ptr<Tree> Parse(std::string src, bool interface) {
    auto t = std::make_unique<Tree>();
    t->src = std::move(src);
    t->interface = interface;
    TSParser *p = ts_parser_new();
    ts_parser_set_language(p, interface ? tree_sitter_ocaml_interface() : tree_sitter_ocaml());
    t->tree = ts_parser_parse_string(p, nullptr, t->src.data(), static_cast<uint32_t>(t->src.size()));
    ts_parser_delete(p);
    return t;
}

std::string_view Text(TSNode n, const std::string &src) {
    if (ts_node_is_null(n)) return {};
    const uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
    if (b > src.size() || e > src.size() || e < b) return {};
    return std::string_view(src).substr(b, e - b);
}

std::string Type(TSNode n) { return ts_node_is_null(n) ? std::string() : std::string(ts_node_type(n)); }

OcamlLspRange RangeOf(TSNode n) {
    OcamlLspRange r;
    const TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
    r.line = static_cast<int>(s.row);
    r.col = static_cast<int>(s.column);
    r.end_line = static_cast<int>(e.row);
    r.end_col = static_cast<int>(e.column);
    return r;
}

TSNode Field(TSNode n, const char *name) {
    return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::char_traits<char>::length(name)));
}

TSNode ChildOfType(TSNode n, const char *type) {
    const uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_named_child(n, i);
        if (std::strcmp(ts_node_type(c), type) == 0) return c;
    }
    return TSNode{};
}

bool HasToken(TSNode n, const char *tok) {
    const uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_child(n, i);
        if (!ts_node_is_named(c) && std::strcmp(ts_node_type(c), tok) == 0) return true;
    }
    return false;
}

std::string Collapse(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::vector<std::string> SplitPath(std::string_view dotted) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : dotted) {
        if (c == '.') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != ' ' && c != '\n' && c != '\t') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

namespace {

bool IsNull(TSNode n) { return ts_node_is_null(n); }

/** The node's text with continuation lines dedented by the node's own start column. */
std::string RawText(TSNode n, const std::string &src) {
    const std::string_view t = Text(n, src);
    const size_t col = ts_node_start_point(n).column;
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i <= t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string_view::npos) j = t.size();
        std::string_view line = t.substr(i, j - i);
        if (!first) {
            size_t k = 0;
            while (k < line.size() && k < col && line[k] == ' ') k++;
            line.remove_prefix(k);
            out += '\n';
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
        out += line;
        first = false;
        i = j + 1;
    }
    return out;
}

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

/** A display name for a value_name / parenthesized_operator node. */
std::string NameText(TSNode n, const std::string &src) {
    if (IsNull(n)) return std::string();
    if (Type(n) == "parenthesized_operator") return Collapse(Text(n, src));
    return std::string(Text(n, src));
}

/** The `[@@deprecated "msg"]` attribute's message (or "deprecated" when there is none). */
bool DeprecatedAttribute(TSNode item, const std::string &src, std::string *msg) {
    const uint32_t count = ts_node_named_child_count(item);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_named_child(item, i);
        if (Type(c) != "item_attribute" && Type(c) != "attribute") continue;
        const std::string id(Text(ChildOfType(c, "attribute_id"), src));
        if (id != "deprecated" && id != "ocaml.deprecated") continue;
        TSNode payload = ChildOfType(c, "attribute_payload");
        std::string m;
        if (!IsNull(payload)) {
            std::string p(Text(payload, src));
            const size_t q1 = p.find('"'), q2 = p.rfind('"');
            if (q1 != std::string::npos && q2 > q1) m = p.substr(q1 + 1, q2 - q1 - 1);
        }
        *msg = m;
        return true;
    }
    return false;
}

void MarkDeprecated(Item *it, TSNode node, const std::string &src) {
    std::string msg;
    if (DeprecatedAttribute(node, src, &msg)) {
        it->is_deprecated = true;
        it->deprecated = msg;
    }
}

/** Value names bound by a pattern (`x`, `(a, b)`, `{ f; g = h }`), in source order. */
void PatternNames(TSNode n, std::vector<TSNode> *out) {
    if (IsNull(n)) return;
    const std::string t = Type(n);
    if (t == "value_name" || t == "value_pattern") {
        out->push_back(n);
        return;
    }
    if (t == "parenthesized_operator") {
        out->push_back(n);
        return;
    }
    const uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; i++) {
        TSNode c = ts_node_named_child(n, i);
        const std::string ct = Type(c);
        if (ct == "type_constructor_path" || ct == "constructor_path" || ct == "field_path" || ct == "module_path" ||
            ct.find("type") != std::string::npos)
            continue;
        PatternNames(c, out);
    }
}

std::string ModuleTypeText(TSNode mt, const std::string &src) { return Collapse(Text(mt, src)); }

std::string TrimText(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

/** @p s without its comments, and without the lines they leave empty. */
std::string StripCommentsText(const std::string &s) {
    std::string out;
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (in_string) {
            if (depth == 0) out += s[i];
            if (s[i] == '\\' && i + 1 < s.size()) {
                if (depth == 0) out += s[i + 1];
                i++;
            } else if (s[i] == '"') {
                in_string = false;
            }
            continue;
        }
        if (s.compare(i, 2, "(*") == 0 && s.compare(i, 3, "(*)") != 0) {
            depth++;
            i++;
            continue;
        }
        if (depth > 0 && s.compare(i, 2, "*)") == 0) {
            depth--;
            i++;
            continue;
        }
        if (depth == 0) {
            if (s[i] == '"') in_string = true;
            out += s[i];
        }
    }
    std::string res;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (TrimText(line).empty()) continue;
        if (!res.empty()) res += '\n';
        res += line;
    }
    return res;
}

/** A type's text, comments removed, whitespace collapsed. */
std::string TypeText(TSNode n, const std::string &src) { return Collapse(StripCommentsText(std::string(Text(n, src)))); }

class Indexer {
public:
    Indexer(const std::string &src, bool interface) : src_(src), interface_(interface) {}

    std::shared_ptr<Module> Run(TSNode container) {
        auto mod = std::make_shared<Module>();
        mod->interface = interface_;
        // Entries: one per item node, with the items it produced.
        struct Entry {
            TSNode node;
            size_t first = 0, last = 0;  // item index range [first, last)
            bool is_comment = false;
            std::string doc;  // for doc comments
            bool is_doc = false;
        };
        std::vector<Entry> entries;
        const uint32_t count = ts_node_named_child_count(container);
        for (uint32_t i = 0; i < count; i++) {
            TSNode c = ts_node_named_child(container, i);
            Entry e;
            e.node = c;
            if (Type(c) == "comment") {
                e.is_comment = true;
                e.doc = DocCommentText(Text(c, src_));
                e.is_doc = !e.doc.empty();
                entries.push_back(e);
                continue;
            }
            e.first = mod->items.size();
            Item1(c, mod.get());
            e.last = mod->items.size();
            entries.push_back(e);
        }
        // Attach doc comments (odoc's rules: right after an item on the same line or the next
        // with a blank line after, else right before the next item, else floating; the first
        // floating doc comment before any item is the module's preamble).
        std::vector<std::string> pre(mod->items.size()), post(mod->items.size());
        bool seen_item = false;
        for (size_t k = 0; k < entries.size(); k++) {
            const Entry &e = entries[k];
            if (!e.is_comment) {
                if (e.last > e.first) seen_item = true;
                continue;
            }
            if (!e.is_doc) continue;
            const Entry *prev = k > 0 && !entries[k - 1].is_comment ? &entries[k - 1] : nullptr;
            const Entry *next = k + 1 < entries.size() && !entries[k + 1].is_comment ? &entries[k + 1] : nullptr;
            const uint32_t cs = ts_node_start_point(e.node).row, ce = ts_node_end_point(e.node).row;
            const bool same_line = prev && cs == ts_node_end_point(prev->node).row;
            const bool adj_prev = prev && cs <= ts_node_end_point(prev->node).row + 1;
            const bool adj_next = next && ts_node_start_point(next->node).row <= ce + 1;
            const Entry *target = nullptr;
            bool is_post = false;
            if (same_line || (adj_prev && !adj_next)) {
                target = prev;
                is_post = true;
            } else if (adj_next) {
                target = next;
            } else if (adj_prev) {
                target = prev;
                is_post = true;
            }
            if (target && target->last > target->first) {
                if (is_post && same_line) {
                    // `| B (** doc *)` closing a type: the doc belongs to its last constructor / field.
                    for (size_t j = target->last; j-- > target->first + 1;) {
                        Item &it = mod->items[j];
                        if ((it.kind == ItemKind::Constructor || it.kind == ItemKind::Field) &&
                            it.range.end_line == static_cast<int>(cs) && it.doc.empty()) {
                            it.doc = e.doc;
                            target = nullptr;
                        }
                        break;
                    }
                    if (!target) continue;
                }
                // The doc goes to the entry's main items (not its constructors / fields).
                for (size_t j = target->first; j < target->last; j++) {
                    const ItemKind kd = mod->items[j].kind;
                    if (j > target->first && (kd == ItemKind::Constructor || kd == ItemKind::Field)) continue;
                    (is_post ? post : pre)[j] = e.doc;
                }
            } else if (!target && !seen_item && mod->doc.empty()) {
                mod->doc = e.doc;
            }
        }
        for (size_t j = 0; j < mod->items.size(); j++) {
            Item &it = mod->items[j];
            std::string d = pre[j];
            if (!post[j].empty()) d = d.empty() ? post[j] : d + "\n\n" + post[j];
            if (!d.empty()) it.doc = it.doc.empty() ? d : d + "\n\n" + it.doc;
            if (!it.is_deprecated && it.doc.find("@deprecated") != std::string::npos) {
                it.is_deprecated = true;
                const size_t p = it.doc.find("@deprecated") + 11;
                size_t e = it.doc.find("\n@", p);
                it.deprecated = Trim(it.doc.substr(p, e == std::string::npos ? std::string::npos : e - p));
            }
        }
        return mod;
    }

private:
    const std::string &src_;
    bool interface_;

    Item Base(ItemKind kind, const std::string &name, TSNode node, TSNode sel) {
        Item it;
        it.kind = kind;
        it.name = name;
        it.range = RangeOf(node);
        it.sel = IsNull(sel) ? it.range : RangeOf(sel);
        return it;
    }

    void Item1(TSNode c, Module *mod) {
        const std::string t = Type(c);
        if (t == "value_definition") {
            Values(c, mod);
        } else if (t == "value_specification" || t == "external") {
            TSNode name = ChildOfType(c, "value_name");
            if (IsNull(name)) name = ChildOfType(c, "parenthesized_operator");
            if (IsNull(name)) return;
            Item it = Base(t == "external" ? ItemKind::External : ItemKind::Value, NameText(name, src_), c, name);
            TSNode ty = Field(c, "type");
            it.type = TypeText(ty, src_);
            if (t == "external") {
                it.signature = "external " + it.name + " : " + it.type;
                TSNode s = ChildOfType(c, "string");
                if (!IsNull(s)) it.signature += " = " + std::string(Text(s, src_));
            } else {
                it.signature = "val " + it.name + " : " + it.type;
            }
            MarkDeprecated(&it, c, src_);
            mod->items.push_back(std::move(it));
        } else if (t == "type_definition") {
            Types(c, mod);
        } else if (t == "exception_definition") {
            TSNode decl = ChildOfType(c, "constructor_declaration");
            TSNode name = IsNull(decl) ? decl : ChildOfType(decl, "constructor_name");
            if (IsNull(name)) return;
            Item it = Base(ItemKind::Exception, std::string(Text(name, src_)), c, name);
            it.signature = Collapse(Text(c, src_));
            MarkDeprecated(&it, c, src_);
            mod->items.push_back(std::move(it));
        } else if (t == "module_definition") {
            const uint32_t n = ts_node_named_child_count(c);
            for (uint32_t i = 0; i < n; i++) {
                TSNode b = ts_node_named_child(c, i);
                if (Type(b) == "module_binding") ModuleBinding(c, b, mod);
            }
        } else if (t == "module_type_definition") {
            TSNode name = ChildOfType(c, "module_type_name");
            if (IsNull(name)) return;
            Item it = Base(ItemKind::ModuleType, std::string(Text(name, src_)), c, name);
            it.signature = "module type " + it.name;
            TSNode body = Field(c, "body");
            ModuleTypeInto(body, &it);
            MarkDeprecated(&it, c, src_);
            mod->items.push_back(std::move(it));
        } else if (t == "include_module" || t == "include_module_type") {
            TSNode m = Field(c, "module");
            if (IsNull(m)) m = Field(c, "module_type");
            if (IsNull(m)) m = ts_node_named_child(c, 0);
            Include(m, mod);
        } else if (t == "class_definition" || t == "class_type_definition") {
            const uint32_t n = ts_node_named_child_count(c);
            for (uint32_t i = 0; i < n; i++) {
                TSNode b = ts_node_named_child(c, i);
                if (Type(b) != "class_binding" && Type(b) != "class_type_binding") continue;
                TSNode name = ChildOfType(b, "class_name");
                if (IsNull(name)) name = ChildOfType(b, "class_type_name");
                if (IsNull(name)) continue;
                Item it = Base(ItemKind::Class, std::string(Text(name, src_)), c, name);
                it.signature = (t == "class_definition" ? "class " : "class type ") + it.name;
                it.members = std::make_shared<Module>();
                Methods(b, it.members.get());
                mod->items.push_back(std::move(it));
            }
        }
    }

    void Methods(TSNode n, Module *mod) {
        const uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; i++) {
            TSNode c = ts_node_named_child(n, i);
            const std::string t = Type(c);
            if (t == "method_definition" || t == "method_specification") {
                TSNode name = ChildOfType(c, "method_name");
                if (IsNull(name)) continue;
                Item it = Base(ItemKind::Method, std::string(Text(name, src_)), c, name);
                TSNode ty = Field(c, "type");
                it.type = TypeText(ty, src_);
                it.signature = "method " + it.name + (it.type.empty() ? std::string() : " : " + it.type);
                mod->items.push_back(std::move(it));
            } else if (t != "comment") {
                Methods(c, mod);
            }
        }
    }

    void Values(TSNode def, Module *mod) {
        const uint32_t count = ts_node_named_child_count(def);
        for (uint32_t i = 0; i < count; i++) {
            TSNode b = ts_node_named_child(def, i);
            if (Type(b) != "let_binding") continue;
            TSNode pat = Field(b, "pattern");
            std::vector<TSNode> names;
            PatternNames(pat, &names);
            if (names.empty()) continue;
            // The head: `let f x ~y : t` (everything before the body's `=`).
            TSNode body = Field(b, "body");
            std::string head;
            {
                const uint32_t s = ts_node_start_byte(b);
                uint32_t e = IsNull(body) ? ts_node_end_byte(b) : ts_node_start_byte(body);
                std::string h = Collapse(std::string_view(src_).substr(s, e - s));
                while (!h.empty() && (h.back() == '=' || h.back() == ' ')) h.pop_back();
                head = h;
            }
            std::vector<std::string> params;
            TSNode type_node = Field(b, "type");
            const uint32_t nc = ts_node_named_child_count(b);
            for (uint32_t k = 0; k < nc; k++) {
                TSNode p = ts_node_named_child(b, k);
                if (Type(p) == "parameter") params.push_back(Collapse(Text(p, src_)));
            }
            for (TSNode name : names) {
                Item it = Base(ItemKind::Value, NameText(name, src_), b, name);
                it.signature = (names.size() == 1 ? "let " + head : "let " + it.name);
                if (names.size() == 1) {
                    it.param_names = params;
                    if (params.empty() && !IsNull(type_node)) it.type = TypeText(type_node, src_);
                }
                MarkDeprecated(&it, def, src_);
                mod->items.push_back(std::move(it));
            }
        }
    }

    void Types(TSNode def, Module *mod) {
        const uint32_t count = ts_node_named_child_count(def);
        for (uint32_t i = 0; i < count; i++) {
            TSNode b = ts_node_named_child(def, i);
            if (Type(b) != "type_binding") continue;
            TSNode name = Field(b, "name");
            if (IsNull(name)) continue;
            Item it = Base(ItemKind::Type, std::string(Text(name, src_)), def, name);
            std::string raw = RawText(b, src_);
            // Docs inside the definition do not belong in the signature.
            it.signature = (i == 0 || count == 1 ? "type " : "and ") + StripComments(raw);
            if (it.signature.rfind("and ", 0) == 0) it.signature = "type " + it.signature.substr(4);
            MarkDeprecated(&it, def, src_);
            const std::string type_name = it.name;
            mod->items.push_back(std::move(it));
            Inner(b, type_name, mod);
        }
    }

    static std::string StripComments(const std::string &s) { return StripCommentsText(s); }

    /** Constructors and fields of a type binding, with their own doc comments. */
    void Inner(TSNode n, const std::string &type_name, Module *mod) {
        const uint32_t count = ts_node_named_child_count(n);
        // Where a variant / record's members live: collect them in order with the comments.
        std::vector<TSNode> seq;
        for (uint32_t i = 0; i < count; i++) {
            TSNode c = ts_node_named_child(n, i);
            const std::string t = Type(c);
            if (t == "variant_declaration" || t == "record_declaration") {
                const uint32_t m = ts_node_named_child_count(c);
                for (uint32_t k = 0; k < m; k++) seq.push_back(ts_node_named_child(c, k));
            }
        }
        // Comments after the last member that tree-sitter left outside the declaration are
        // handled by Run() (same line as the type's end).
        std::vector<size_t> idx(seq.size(), SIZE_MAX);
        for (size_t k = 0; k < seq.size(); k++) {
            TSNode c = seq[k];
            const std::string t = Type(c);
            if (t == "constructor_declaration") {
                TSNode name = ChildOfType(c, "constructor_name");
                if (IsNull(name)) continue;
                Item it = Base(ItemKind::Constructor, std::string(Text(name, src_)), c, name);
                it.signature = StripComments(Collapse(Text(c, src_)));
                it.owner_type = type_name;
                MarkDeprecated(&it, c, src_);
                idx[k] = mod->items.size();
                mod->items.push_back(std::move(it));
            } else if (t == "field_declaration") {
                TSNode name = ChildOfType(c, "field_name");
                if (IsNull(name)) continue;
                Item it = Base(ItemKind::Field, std::string(Text(name, src_)), c, name);
                it.signature = StripComments(Collapse(Text(c, src_)));
                TSNode ty = Field(c, "type");
                if (!IsNull(ty)) it.type = TypeText(ty, src_);
                it.owner_type = type_name;
                MarkDeprecated(&it, c, src_);
                idx[k] = mod->items.size();
                mod->items.push_back(std::move(it));
            }
        }
        // Pass 1: a doc comment on a member's own last line is that member's. Pass 2: a comment
        // on its own line belongs to the member above when indented deeper than the next one
        // (`| Green\n      (** doc *)`), else to the member below.
        for (int pass = 1; pass <= 2; pass++) {
            for (size_t k = 0; k < seq.size(); k++) {
                if (Type(seq[k]) != "comment") continue;
                const std::string doc = DocCommentText(Text(seq[k], src_));
                if (doc.empty()) continue;
                const TSPoint at = ts_node_start_point(seq[k]);
                const bool has_prev = k > 0 && idx[k - 1] != SIZE_MAX;
                const bool has_next = k + 1 < seq.size() && idx[k + 1] != SIZE_MAX;
                const bool same_line = has_prev && ts_node_end_point(seq[k - 1]).row == at.row;
                size_t target = SIZE_MAX;
                if (pass == 1) {
                    if (same_line) target = idx[k - 1];
                } else if (!same_line) {
                    if (has_prev && (!has_next || at.column > ts_node_start_point(seq[k + 1]).column))
                        target = idx[k - 1];
                    else if (has_next)
                        target = idx[k + 1];
                }
                if (target != SIZE_MAX && mod->items[target].doc.empty()) mod->items[target].doc = doc;
            }
        }
    }

    void ModuleTypeInto(TSNode mt, Item *it) {
        if (IsNull(mt)) return;
        const std::string t = Type(mt);
        if (t == "signature") {
            it->members = Indexer(src_, true).Run(mt);
            it->signature += " : sig ... end";
        } else if (t == "module_type_path") {
            it->module_type = ModuleTypeText(mt, src_);
            it->signature += " : " + it->module_type;
        } else if (t == "module_type_of") {
            TSNode m = Field(mt, "module");
            if (IsNull(m)) m = ChildOfType(mt, "module_path");
            it->alias = Collapse(Text(m, src_));
            it->signature += " : module type of " + it->alias;
        } else if (t == "module_type_constraint") {
            ModuleTypeInto(Field(mt, "module_type"), it);
        } else if (t == "functor_type") {
            it->signature += " : " + Collapse(Text(mt, src_));
            TSNode body = Field(mt, "body");
            if (IsNull(body)) body = ts_node_named_child(mt, ts_node_named_child_count(mt) - 1);
            Item tmp;
            ModuleTypeInto(body, &tmp);
            it->members = tmp.members;
            if (it->members) it->members->opaque = true;
        } else if (t == "parenthesized_module_type") {
            ModuleTypeInto(ts_node_named_child(mt, 0), it);
        }
    }

    void ModuleExprInto(TSNode body, Item *it) {
        if (IsNull(body)) return;
        const std::string t = Type(body);
        if (t == "structure") {
            if (!it->members) it->members = Indexer(src_, false).Run(body);
        } else if (t == "module_path" || t == "extended_module_path") {
            if (it->members || !it->module_type.empty()) return;  // `module M : S = Foo`: the signature wins
            it->alias = Collapse(Text(body, src_));
            if (it->signature.find(':') == std::string::npos) it->signature += " = " + it->alias;
        } else if (t == "typed_module_expression") {
            ModuleTypeInto(Field(body, "module_type"), it);
            ModuleExprInto(Field(body, "module"), it);
        } else if (t == "functor") {
            TSNode b = Field(body, "body");
            if (IsNull(b)) b = ts_node_named_child(body, ts_node_named_child_count(body) - 1);
            Item tmp;
            ModuleExprInto(b, &tmp);
            if (!it->members) it->members = tmp.members;
            if (it->members) it->members->opaque = true;
        } else if (t == "parenthesized_module_expression") {
            ModuleExprInto(ts_node_named_child(body, 0), it);
        } else if (!it->members) {
            it->members = std::make_shared<Module>();
            it->members->opaque = true;  // a functor application, (val ...), ...
        }
    }

    void ModuleBinding(TSNode def, TSNode b, Module *mod) {
        TSNode name = ChildOfType(b, "module_name");
        if (IsNull(name)) return;
        Item it = Base(ItemKind::Module, std::string(Text(name, src_)), def, name);
        it.signature = "module " + it.name;
        const bool functor = !IsNull(ChildOfType(b, "module_parameter"));
        ModuleTypeInto(Field(b, "module_type"), &it);
        ModuleExprInto(Field(b, "body"), &it);
        if (functor) {
            if (!it.members) it.members = std::make_shared<Module>();
            it.members->opaque = true;
            std::string params;
            const uint32_t n = ts_node_named_child_count(b);
            for (uint32_t i = 0; i < n; i++) {
                TSNode p = ts_node_named_child(b, i);
                if (Type(p) == "module_parameter") params += " " + Collapse(Text(p, src_));
            }
            it.signature = "module " + it.name + params + it.signature.substr(7 + it.name.size());
        }
        MarkDeprecated(&it, def, src_);
        mod->items.push_back(std::move(it));
    }

    void Include(TSNode m, Module *mod) {
        if (IsNull(m)) return;
        const std::string t = Type(m);
        if (t == "module_path" || t == "extended_module_path") {
            mod->includes.push_back(Collapse(Text(m, src_)));
        } else if (t == "module_type_path") {
            mod->include_types.push_back(Collapse(Text(m, src_)));
        } else if (t == "module_type_of") {
            TSNode p = Field(m, "module");
            if (IsNull(p)) p = ChildOfType(m, "module_path");
            if (!IsNull(p)) mod->includes.push_back(Collapse(Text(p, src_)));
        } else if (t == "module_type_constraint" || t == "typed_module_expression") {
            TSNode inner = Field(m, "module_type");
            if (t == "typed_module_expression") inner = Field(m, "module");
            Include(inner, mod);
        } else if (t == "signature" || t == "structure") {
            auto inner = Indexer(src_, t == "signature").Run(m);
            for (Item &it : inner->items) mod->items.push_back(std::move(it));
            for (auto &s : inner->includes) mod->includes.push_back(s);
            for (auto &s : inner->include_types) mod->include_types.push_back(s);
        } else if (t == "parenthesized_module_expression" || t == "parenthesized_module_type") {
            Include(ts_node_named_child(m, 0), mod);
        } else {
            mod->opaque = true;  // include F(X): unknown members
        }
    }
};

}  // namespace

std::string DocCommentText(std::string_view c) {
    if (c.size() < 5 || c.compare(0, 3, "(**") != 0 || c.compare(c.size() - 2, 2, "*)") != 0) return std::string();
    if (c[3] == '*' || c == "(**)" || c.compare(0, 7, "(**/**)") == 0) return std::string();
    std::string_view inner = c.substr(3, c.size() - 5);
    // Dedent continuation lines by their common indentation.
    std::vector<std::string_view> lines;
    size_t i = 0;
    while (i <= inner.size()) {
        size_t j = inner.find('\n', i);
        if (j == std::string_view::npos) j = inner.size();
        lines.push_back(inner.substr(i, j - i));
        i = j + 1;
    }
    size_t common = SIZE_MAX;
    for (size_t k = 1; k < lines.size(); k++) {
        const std::string_view l = lines[k];
        size_t n = 0;
        while (n < l.size() && (l[n] == ' ' || l[n] == '\t')) n++;
        if (n == l.size()) continue;
        common = std::min(common, n);
    }
    if (common == SIZE_MAX) common = 0;
    std::string out;
    for (size_t k = 0; k < lines.size(); k++) {
        std::string_view l = lines[k];
        if (k > 0) {
            l.remove_prefix(std::min(common, l.size()));
            out += '\n';
        }
        out += l;
    }
    return Trim(out);
}

void SetPaths(Module *mod, const std::string &path) {
    mod->path = path;
    for (Item &it : mod->items) {
        if (it.members) SetPaths(it.members.get(), path);
    }
}

std::shared_ptr<Module> IndexNode(TSNode container, const std::string &src, bool interface) {
    return Indexer(src, interface).Run(container);
}

std::shared_ptr<Module> IndexSource(const std::string &src, bool interface, const std::string &path,
                                    const std::string &name) {
    auto tree = Parse(src, interface);
    auto mod = IndexNode(tree->Root(), tree->src, interface);
    mod->name = name;
    SetPaths(mod.get(), path);
    return mod;
}

// --- the load path -----------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

std::string g_workspace_root;

struct DirIndex {
    std::map<std::string, std::vector<std::string>> by_base;  // "list" -> [.../list.mli, .../list.ml]
};

std::map<std::string, std::shared_ptr<DirIndex>> g_dir_cache;  // key: kind + dir

struct FileEntry {
    long long mtime = 0;
    long long size = 0;
    std::shared_ptr<const Module> mod;
};
std::map<std::string, FileEntry> g_file_cache;

bool g_stdlib_dir_known = false;
std::string g_stdlib_dir;
bool g_findlib_known = false;
std::vector<std::string> g_findlib_path;

bool IsDir(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

std::string GetEnv(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::vector<std::string> SplitList(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
#if defined(_WIN32)
        if (c == ';') {
#else
        if (c == ':') {
#endif
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool OnPath(const std::string &exe) {
    for (const std::string &d : SplitList(GetEnv("PATH"))) {
        std::error_code ec;
        const fs::path p = fs::path(d) / exe;
        if (fs::is_regular_file(p, ec)) return true;
    }
    return false;
}

std::vector<std::string> RunLines(const std::string &cmd) {
    std::vector<std::string> out;
    FILE *f = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!f) return out;
    std::string cur;
    char buf[512];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) cur.append(buf, got);
    pclose(f);
    std::istringstream in(cur);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool IsSourceExt(const std::string &ext) { return ext == ".ml" || ext == ".mli"; }

std::string Lower1(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

void AddFile(DirIndex *idx, const fs::path &p) {
    const std::string ext = p.extension().string();
    if (!IsSourceExt(ext)) return;
    idx->by_base[Lower1(p.stem().string())].push_back(p.string());
}

/** A directory's .ml/.mli files, `depth` levels down (library dirs: dir, dir/pkg, dir/pkg/sub). */
std::shared_ptr<DirIndex> IndexDir(const std::string &dir, int depth, bool skip_build) {
    const std::string key = std::to_string(depth) + (skip_build ? "w:" : "l:") + dir;
    auto it = g_dir_cache.find(key);
    if (it != g_dir_cache.end()) return it->second;
    auto idx = std::make_shared<DirIndex>();
    size_t budget = 20000;
    std::function<void(const fs::path &, int)> walk = [&](const fs::path &d, int left) {
        std::error_code ec;
        fs::directory_iterator di(d, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        std::vector<fs::path> subdirs;
        for (const auto &e : di) {
            if (budget == 0) return;
            budget--;
            std::error_code ec2;
            if (e.is_directory(ec2)) {
                const std::string n = e.path().filename().string();
                if (left <= 0) continue;
                if (skip_build && (n == "_build" || n == "_opam" || n == "_esy" || n == "node_modules" ||
                                   (!n.empty() && n[0] == '.')))
                    continue;
                subdirs.push_back(e.path());
            } else {
                AddFile(idx.get(), e.path());
            }
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (const fs::path &s : subdirs) walk(s, left - 1);
    };
    walk(fs::path(dir), depth);
    for (auto &kv : idx->by_base) std::sort(kv.second.begin(), kv.second.end());
    g_dir_cache[key] = idx;
    return idx;
}

/** The project root of a file: the workspace root when the file is inside it, else the
 *  nearest ancestor with dune-project / .git / *.opam, else the file's directory. */
std::string ProjectRoot(const Env &env) {
    std::error_code ec;
    if (!g_workspace_root.empty()) {
        const std::string root = fs::weakly_canonical(g_workspace_root, ec).string();
        if (env.dir.empty() || env.dir.rfind(root, 0) == 0) return root;
    }
    if (env.dir.empty()) return std::string();
    fs::path d(env.dir);
    for (int i = 0; i < 8 && !d.empty(); i++) {
        if (fs::exists(d / "dune-project", ec) || fs::exists(d / ".git", ec)) return d.string();
        const fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return std::string();
}

std::vector<std::string> Candidates(const std::string &name) {
    std::vector<std::string> out;
    out.push_back(Lower1(name));
    const size_t dd = name.rfind("__");
    if (dd != std::string::npos && dd + 2 < name.size()) out.push_back(Lower1(name.substr(dd + 2)));
    return out;
}

/** The .ml/.mli pair for @p base among @p files (paths), preferring one directory. */
bool PickPair(const std::vector<std::string> &files, ModuleFile *out) {
    if (files.empty()) return false;
    // Group by directory; take the first directory that has an .mli, else the first.
    std::map<std::string, ModuleFile> by_dir;
    std::vector<std::string> order;
    for (const std::string &f : files) {
        const fs::path p(f);
        const std::string dir = p.parent_path().string();
        if (!by_dir.count(dir)) order.push_back(dir);
        (p.extension() == ".mli" ? by_dir[dir].mli : by_dir[dir].ml) = f;
    }
    for (const std::string &d : order) {
        if (!by_dir[d].mli.empty()) {
            *out = by_dir[d];
            return true;
        }
    }
    *out = by_dir[order.front()];
    return true;
}

}  // namespace

void SetWorkspaceRoot(const std::string &root) { g_workspace_root = root; }

void ResetCaches() {
    g_dir_cache.clear();
    g_file_cache.clear();
    g_stdlib_dir_known = false;
    g_stdlib_dir.clear();
    g_findlib_known = false;
    g_findlib_path.clear();
}

std::string StdlibDir() {
    // The environment overrides are read every time (tests change them).
    if (const char *v = std::getenv("MEP_OCAML_STDLIB")) return IsDir(v) ? std::string(v) : std::string();
    const std::string ocamllib = GetEnv("OCAMLLIB");
    if (IsDir(ocamllib)) return ocamllib;
    if (!g_stdlib_dir_known) {
        g_stdlib_dir_known = true;
        std::vector<std::string> lines;
        if (OnPath("ocamlfind")) lines = RunLines("ocamlfind ocamlc -where");
        if (lines.empty() && OnPath("ocamlc")) lines = RunLines("ocamlc -where");
        if (!lines.empty() && IsDir(lines[0])) g_stdlib_dir = lines[0];
    }
    return g_stdlib_dir;
}

std::vector<std::string> LibraryDirs(const Env &env) {
    std::vector<std::string> dirs;
    auto add = [&](const std::string &d) {
        if (!IsDir(d)) return;
        std::error_code ec;
        const std::string c = fs::weakly_canonical(d, ec).string();
        if (std::find(dirs.begin(), dirs.end(), c) == dirs.end()) dirs.push_back(c);
    };
    for (const std::string &d : env.include_dirs) add(d);
    for (const std::string &d : SplitList(GetEnv("MEP_OCAML_PATH"))) add(d);
    const std::string root = ProjectRoot(env);
    if (!root.empty()) add((fs::path(root) / "_opam" / "lib").string());
    if (!env.dir.empty()) {
        fs::path d(env.dir);
        for (int i = 0; i < 8 && !d.empty(); i++) {
            add((d / "_opam" / "lib").string());
            if (d.parent_path() == d) break;
            d = d.parent_path();
        }
    }
    const std::string prefix = GetEnv("OPAM_SWITCH_PREFIX");
    if (!prefix.empty()) add((fs::path(prefix) / "lib").string());
    for (const std::string &d : SplitList(GetEnv("OCAMLPATH"))) add(d);
    if (std::getenv("MEP_OCAML_NO_FINDLIB") == nullptr) {
        if (!g_findlib_known) {
            g_findlib_known = true;
            if (OnPath("ocamlfind")) g_findlib_path = RunLines("ocamlfind printconf path");
        }
        for (const std::string &d : g_findlib_path) add(d);
    }
    return dirs;
}

ModuleFile FindModuleFile(const std::string &name, const Env &env) {
    ModuleFile out;
    if (name.empty() || !std::isupper(static_cast<unsigned char>(name[0]))) return out;
    const std::vector<std::string> cands = Candidates(name);
    auto try_index = [&](const DirIndex &idx) {
        for (const std::string &c : cands) {
            auto it = idx.by_base.find(c);
            if (it != idx.by_base.end() && PickPair(it->second, &out)) return true;
        }
        return false;
    };
    // 1. The document's directory, then -I directories.
    if (!env.dir.empty() && try_index(*IndexDir(env.dir, 0, true))) return out;
    for (const std::string &d : env.include_dirs) {
        if (IsDir(d) && try_index(*IndexDir(d, 0, true))) return out;
    }
    // 2. The workspace / project.
    const std::string root = ProjectRoot(env);
    if (!root.empty() && try_index(*IndexDir(root, 6, true))) return out;
    // 3. The standard library (Stdlib__List is list.mli there).
    const std::string std_dir = StdlibDir();
    if (!std_dir.empty() && try_index(*IndexDir(std_dir, 0, false))) return out;
    // 4. Libraries.
    for (const std::string &d : LibraryDirs(env)) {
        if (try_index(*IndexDir(d, 2, false))) return out;
    }
    return out;
}

std::shared_ptr<const Module> LoadFile(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return nullptr;
    const long long mtime = static_cast<long long>(st.st_mtime);
    const long long size = static_cast<long long>(st.st_size);
    auto it = g_file_cache.find(path);
    if (it != g_file_cache.end() && it->second.mtime == mtime && it->second.size == size) return it->second.mod;
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::ostringstream ss;
    ss << in.rdbuf();
    const fs::path p(path);
    std::string name = p.stem().string();
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    auto mod = IndexSource(ss.str(), p.extension() == ".mli", path, name);
    FileEntry e;
    e.mtime = mtime;
    e.size = size;
    e.mod = mod;
    g_file_cache[path] = e;
    return mod;
}

namespace {

std::shared_ptr<const Module> Fallback(const std::string &name) {
    static std::map<std::string, std::shared_ptr<const Module>> cache;
    std::string n = name;
    if (n.rfind("Stdlib__", 0) == 0) n = n.substr(8);
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;
    std::shared_ptr<const Module> mod;
    for (const auto &kv : FallbackModules()) {
        if (kv.first == n) {
            auto m = IndexSource(kv.second, true, std::string(), n);
            mod = m;
            break;
        }
    }
    cache[n] = mod;
    return mod;
}

}  // namespace

std::shared_ptr<const Module> LoadModule(const std::string &name, const Env &env) {
    if (name == "Stdlib") return StdlibModule();
    const ModuleFile f = FindModuleFile(name, env);
    const std::string path = !f.mli.empty() ? f.mli : f.ml;
    if (!path.empty()) {
        auto m = LoadFile(path);
        if (m) return m;
    }
    if (StdlibDir().empty()) return Fallback(name);
    return nullptr;
}

std::shared_ptr<const Module> StdlibModule() {
    const std::string dir = StdlibDir();
    if (!dir.empty()) {
        auto m = LoadFile((fs::path(dir) / "stdlib.mli").string());
        if (m) return m;
    }
    return Fallback("Stdlib");
}

std::vector<std::string> KnownModules(const Env &env) {
    std::set<std::string> names;
    auto add_index = [&](const DirIndex &idx) {
        for (const auto &kv : idx.by_base) {
            std::string n = kv.first;
            if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])))) continue;
            if (n.find("__") != std::string::npos) continue;  // dune's / the stdlib's mangled units
            if (n == "stdlib") continue;
            n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
            names.insert(n);
        }
    };
    if (!env.dir.empty()) add_index(*IndexDir(env.dir, 0, true));
    for (const std::string &d : env.include_dirs) {
        if (IsDir(d)) add_index(*IndexDir(d, 0, true));
    }
    const std::string root = ProjectRoot(env);
    if (!root.empty()) add_index(*IndexDir(root, 6, true));
    if (auto std_mod = StdlibModule()) {
        for (const Item &it : std_mod->items) {
            if (it.kind == ItemKind::Module) names.insert(it.name);
        }
    }
    for (const std::string &d : LibraryDirs(env)) add_index(*IndexDir(d, 2, false));
    names.insert("Stdlib");
    return std::vector<std::string>(names.begin(), names.end());
}

// --- resolution --------------------------------------------------------------------

namespace {

constexpr int kMaxDepth = 16;

const Item *OwnModuleItem(const Module *owner, const std::string &name, const Item *except) {
    if (!owner) return nullptr;
    for (size_t i = owner->items.size(); i-- > 0;) {
        const Item &it = owner->items[i];
        if (&it != except && it.kind == ItemKind::Module && it.name == name) return &it;
    }
    return nullptr;
}

/** A module path seen from inside @p owner (its own submodules first), then globally. */
std::shared_ptr<const Module> ResolveIn(const Module *owner, const std::vector<std::string> &path, const Item *except,
                                        const Scope &scope, int depth) {
    if (path.empty() || depth > kMaxDepth) return nullptr;
    if (const Item *it = OwnModuleItem(owner, path[0], except)) {
        auto m = ModuleOfItem(*it, owner, scope, depth + 1);
        for (size_t i = 1; m && i < path.size(); i++) {
            Found f = FindItem(m, path[i], {ItemKind::Module}, scope, depth + 1);
            m = f.item ? ModuleOfItem(*f.item, f.module.get(), scope, depth + 1) : nullptr;
        }
        if (m) return m;
    }
    return ResolveModulePath(path, scope, depth + 1);
}

std::shared_ptr<const Module> ResolveModuleType(const Module *owner, const std::string &dotted, const Scope &scope,
                                                int depth) {
    std::vector<std::string> path = SplitPath(dotted);
    if (path.empty() || depth > kMaxDepth) return nullptr;
    const std::string last = path.back();
    path.pop_back();
    std::shared_ptr<const Module> holder;
    if (path.empty()) {
        // Own module types first, then the document's, then Stdlib's.
        if (owner) {
            for (size_t i = owner->items.size(); i-- > 0;) {
                const Item &it = owner->items[i];
                if (it.kind == ItemKind::ModuleType && it.name == last) return ModuleOfItem(it, owner, scope, depth + 1);
            }
        }
        if (scope.local) {
            Found f = FindItem(scope.local, last, {ItemKind::ModuleType}, scope, depth + 1);
            if (f.item) return ModuleOfItem(*f.item, f.module.get(), scope, depth + 1);
        }
        return nullptr;
    }
    holder = ResolveIn(owner, path, nullptr, scope, depth + 1);
    if (!holder) return nullptr;
    Found f = FindItem(holder, last, {ItemKind::ModuleType}, scope, depth + 1);
    return f.item ? ModuleOfItem(*f.item, f.module.get(), scope, depth + 1) : nullptr;
}

}  // namespace

std::shared_ptr<const Module> ModuleOfItem(const Item &item, const Module *owner, const Scope &scope, int depth) {
    if (depth > kMaxDepth) return nullptr;
    if (item.members && (!item.members->items.empty() || !item.members->includes.empty() ||
                         !item.members->include_types.empty() || item.alias.empty())) {
        if (!item.module_type.empty() && item.members->items.empty()) {
            if (auto m = ResolveModuleType(owner, item.module_type, scope, depth + 1)) return m;
        }
        // Keep the owner alive alongside: members is owned by the item, itself owned by owner.
        return item.members;
    }
    if (!item.module_type.empty()) {
        if (auto m = ResolveModuleType(owner, item.module_type, scope, depth + 1)) return m;
    }
    if (!item.alias.empty()) return ResolveIn(owner, SplitPath(item.alias), &item, scope, depth + 1);
    return item.members;
}

std::shared_ptr<const Module> ResolveModulePath(const std::vector<std::string> &path, const Scope &scope, int depth) {
    if (path.empty() || depth > kMaxDepth) return nullptr;
    std::shared_ptr<const Module> mod;
    const std::string &first = path[0];
    if (scope.local) {
        Found f = FindItem(scope.local, first, {ItemKind::Module}, Scope{scope.env, nullptr}, depth + 1);
        if (f.item) mod = ModuleOfItem(*f.item, f.module.get(), scope, depth + 1);
    }
    if (!mod && scope.env) mod = LoadModule(first, *scope.env);
    if (!mod) {
        // Stdlib's own submodules (`module List = Stdlib__List`).
        auto std_mod = StdlibModule();
        if (std_mod && std_mod->name != first) {
            Found f = FindItem(std_mod, first, {ItemKind::Module}, Scope{scope.env, nullptr}, depth + 1);
            if (f.item) mod = ModuleOfItem(*f.item, f.module.get(), Scope{scope.env, nullptr}, depth + 1);
        }
    }
    if (!mod) mod = Fallback(first);
    for (size_t i = 1; mod && i < path.size(); i++) {
        Found f = FindItem(mod, path[i], {ItemKind::Module}, scope, depth + 1);
        mod = f.item ? ModuleOfItem(*f.item, f.module.get(), scope, depth + 1) : nullptr;
    }
    return mod;
}

Found FindItem(const std::shared_ptr<const Module> &mod, const std::string &name, const std::set<ItemKind> &kinds,
               const Scope &scope, int depth) {
    Found out;
    if (!mod || depth > kMaxDepth) return out;
    for (size_t i = mod->items.size(); i-- > 0;) {
        const Item &it = mod->items[i];
        if (it.name == name && (kinds.empty() || kinds.count(it.kind))) {
            out.item = &it;
            out.module = mod;
            return out;
        }
    }
    for (size_t i = mod->includes.size(); i-- > 0;) {
        auto inc = ResolveIn(mod.get(), SplitPath(mod->includes[i]), nullptr, scope, depth + 1);
        if (inc == mod) continue;
        Found f = FindItem(inc, name, kinds, scope, depth + 1);
        if (f.item) return f;
    }
    for (size_t i = mod->include_types.size(); i-- > 0;) {
        auto inc = ResolveModuleType(mod.get(), mod->include_types[i], scope, depth + 1);
        if (inc == mod) continue;
        Found f = FindItem(inc, name, kinds, scope, depth + 1);
        if (f.item) return f;
    }
    return out;
}

std::vector<Found> AllItems(const std::shared_ptr<const Module> &mod, const Scope &scope, int depth) {
    std::vector<Found> out;
    if (!mod || depth > kMaxDepth) return out;
    std::set<std::pair<int, std::string>> seen;
    auto add = [&](const Found &f) {
        const auto key = std::make_pair(static_cast<int>(f.item->kind), f.item->name);
        if (seen.insert(key).second) out.push_back(f);
    };
    for (size_t i = mod->items.size(); i-- > 0;) add(Found{&mod->items[i], mod});
    for (size_t i = mod->includes.size(); i-- > 0;) {
        auto inc = ResolveIn(mod.get(), SplitPath(mod->includes[i]), nullptr, scope, depth + 1);
        if (inc == mod) continue;
        for (const Found &f : AllItems(inc, scope, depth + 1)) add(f);
    }
    for (size_t i = mod->include_types.size(); i-- > 0;) {
        auto inc = ResolveModuleType(mod.get(), mod->include_types[i], scope, depth + 1);
        if (inc == mod) continue;
        for (const Found &f : AllItems(inc, scope, depth + 1)) add(f);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace mllsp
