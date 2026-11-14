// Definitions, the load path and the World for mep's Ruby language server
// (ruby_lsp_internal.h).
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>

#include "ruby_lsp_internal.h"

namespace fs = std::filesystem;

namespace rblsp {

Tree::~Tree() {
    if (tree) ts_tree_delete(tree);
}

std::unique_ptr<Tree> Parse(std::string text) {
    auto t = std::make_unique<Tree>();
    t->text = std::move(text);
    TSParser *p = ts_parser_new();
    ts_parser_set_language(p, tree_sitter_ruby());
    t->tree = ts_parser_parse_string(p, nullptr, t->text.data(), static_cast<uint32_t>(t->text.size()));
    ts_parser_delete(p);
    return t;
}

RubyLspRange RangeOf(TSNode n) {
    RubyLspRange r;
    if (ts_node_is_null(n)) return r;
    const TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
    r.line = static_cast<int>(s.row);
    r.col = static_cast<int>(s.column);
    r.end_line = static_cast<int>(e.row);
    r.end_col = static_cast<int>(e.column);
    return r;
}

std::string ParamLabel(const Param &p) {
    switch (p.kind) {
    case ParamKind::Req: return p.name;
    case ParamKind::Opt: return p.name + " = " + p.def;
    case ParamKind::Rest: return "*" + p.name;
    case ParamKind::KeyReq: return p.name + ":";
    case ParamKind::Key: return p.name + ": " + p.def;
    case ParamKind::KeyRest: return "**" + p.name;
    case ParamKind::Block: return "&" + p.name;
    case ParamKind::Forward: return "...";
    case ParamKind::Destructured: return p.name;
    }
    return p.name;
}

std::string Def::Signature() const {
    if (kind != DefKind::Method && kind != DefKind::Alias) return name;
    std::string s = name;
    if (params.empty()) return s;
    s += "(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i) s += ", ";
        s += ParamLabel(params[i]);
    }
    return s + ")";
}

namespace {

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// Splits on top-level commas (outside brackets and quotes).
std::vector<std::string> SplitTop(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

}  // namespace

std::vector<Param> ParseParamList(const std::string &text) {
    std::string s = Trim(text);
    if (!s.empty() && s.front() == '(') {
        int depth = 0;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '(') depth++;
            if (s[i] == ')' && --depth == 0) {
                s = s.substr(1, i - 1);
                break;
            }
        }
    }
    std::vector<Param> out;
    for (const std::string &a : SplitTop(s)) {
        Param p;
        if (a == "...") {
            p.kind = ParamKind::Forward;
            p.name = "...";
        } else if (a.compare(0, 2, "**") == 0) {
            p.kind = ParamKind::KeyRest;
            p.name = a.substr(2);
        } else if (a[0] == '*') {
            p.kind = ParamKind::Rest;
            p.name = a.substr(1);
        } else if (a[0] == '&') {
            p.kind = ParamKind::Block;
            p.name = a.substr(1);
        } else if (a[0] == '(') {
            p.kind = ParamKind::Destructured;
            p.name = a;
        } else {
            const size_t colon = a.find(':');
            const size_t eq = a.find('=');
            if (colon != std::string::npos && (eq == std::string::npos || colon < eq) &&
                (colon + 1 >= a.size() || a[colon + 1] != ':')) {
                p.name = Trim(a.substr(0, colon));
                p.def = Trim(a.substr(colon + 1));
                p.kind = p.def.empty() ? ParamKind::KeyReq : ParamKind::Key;
            } else if (eq != std::string::npos) {
                p.name = Trim(a.substr(0, eq));
                p.def = Trim(a.substr(eq + 1));
                p.kind = ParamKind::Opt;
            } else {
                p.name = a;
            }
        }
        out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Indexing a tree

namespace {

bool MagicComment(const std::string &body) {
    const std::string t = Trim(body);
    for (const char *k : {"frozen_string_literal:", "encoding:", "coding:", "warn_indent:", "shareable_constant_value:",
                          "typed:", "rubocop:", "-*-", "!/", "CMD:", "standard:"}) {
        if (t.find(k) == 0) return true;
    }
    return false;
}

struct Indexer {
    const Tree &t;
    std::shared_ptr<FileIndex> out;
    std::vector<std::string> lines;
    std::vector<bool> comment_line;  // the row is a whole-line `#` comment

    Indexer(const Tree &tree, const std::string &path) : t(tree), out(std::make_shared<FileIndex>()) {
        out->path = path;
        std::string cur;
        for (char c : t.text) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        lines.push_back(cur);
        comment_line.assign(lines.size(), false);
    }

    std::string Txt(TSNode n) const { return Text(t.text, n); }

    void MarkComments(TSNode n) {
        if (Is(n, "comment")) {
            const TSPoint s = ts_node_start_point(n);
            if (s.row < lines.size()) {
                const std::string &l = lines[s.row];
                size_t k = 0;
                while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
                if (k == s.column && k < l.size() && l[k] == '#') comment_line[s.row] = true;
            }
            return;
        }
        const uint32_t n_children = ts_node_child_count(n);
        for (uint32_t i = 0; i < n_children; i++) MarkComments(ts_node_child(n, i));
    }

    // The `#` lines right above @p row, without the `#` and one space.
    std::shared_ptr<YardDoc> DocAbove(uint32_t row) const {
        std::vector<std::string> block;
        int r = static_cast<int>(row) - 1;
        while (r >= 0 && comment_line[static_cast<size_t>(r)]) {
            const std::string &l = lines[static_cast<size_t>(r)];
            const size_t hash = l.find('#');
            std::string body = l.substr(hash + 1);
            if (MagicComment(body)) break;
            if (!body.empty() && body[0] == ' ') body.erase(0, 1);
            block.insert(block.begin(), body);
            r--;
        }
        // `##` opens a YARD block; a lone `#` line on top is not text.
        while (!block.empty() && (Trim(block.front()).empty() || Trim(block.front()) == "#")) block.erase(block.begin());
        if (block.empty()) return nullptr;
        auto d = ParseYard(block);
        return d->Empty() ? nullptr : d;
    }

    struct Ctx {
        std::string owner;
        std::vector<std::string> nesting;
        bool singleton = false;
        Def *ns = nullptr;  // the module/class being defined
    };
    struct BodyState {
        std::string visibility;
        bool module_function = false;
    };

    static std::string Join(const std::string &owner, const std::string &name) {
        if (name.compare(0, 2, "::") == 0) return name.substr(2);
        return owner.empty() ? name : owner + "::" + name;
    }
    static std::string LastSeg(const std::string &fqn) {
        const size_t k = fqn.rfind("::");
        return k == std::string::npos ? fqn : fqn.substr(k + 2);
    }
    static std::string Parent(const std::string &fqn) {
        const size_t k = fqn.rfind("::");
        return k == std::string::npos ? std::string() : fqn.substr(0, k);
    }

    Def *Add(std::unique_ptr<Def> d) {
        d->path = out->path;
        out->defs.push_back(std::move(d));
        return out->defs.back().get();
    }

    std::vector<Param> Params(TSNode params) const {
        std::vector<Param> ps;
        if (ts_node_is_null(params)) return ps;
        const uint32_t n = ts_node_named_child_count(params);
        for (uint32_t i = 0; i < n; i++) {
            TSNode c = ts_node_named_child(params, i);
            const std::string_view ty = NodeType(c);
            Param p;
            if (ty == "identifier") {
                p.name = Txt(c);
            } else if (ty == "optional_parameter") {
                p.kind = ParamKind::Opt;
                p.name = Txt(Field(c, "name"));
                p.def = Txt(Field(c, "value"));
            } else if (ty == "splat_parameter") {
                p.kind = ParamKind::Rest;
                p.name = Txt(Field(c, "name"));
            } else if (ty == "keyword_parameter") {
                TSNode v = Field(c, "value");
                p.kind = ts_node_is_null(v) ? ParamKind::KeyReq : ParamKind::Key;
                p.name = Txt(Field(c, "name"));
                p.def = Txt(v);
            } else if (ty == "hash_splat_parameter") {
                p.kind = ParamKind::KeyRest;
                p.name = Txt(Field(c, "name"));
            } else if (ty == "hash_splat_nil") {
                p.kind = ParamKind::KeyRest;
                p.name = "nil";
            } else if (ty == "block_parameter") {
                p.kind = ParamKind::Block;
                p.name = Txt(Field(c, "name"));
            } else if (ty == "forward_parameter") {
                p.kind = ParamKind::Forward;
                p.name = "...";
            } else if (ty == "destructured_parameter") {
                p.kind = ParamKind::Destructured;
                p.name = Txt(c);
            } else {
                continue;
            }
            if (p.def.size() > 40) p.def = p.def.substr(0, 37) + "...";
            ps.push_back(p);
        }
        return ps;
    }

    void Method(TSNode n, const Ctx &ctx, const BodyState &st, const std::string &visibility_override) {
        const bool singleton_def = Is(n, "singleton_method");
        std::string owner = ctx.owner;
        bool singleton = ctx.singleton;
        if (singleton_def) {
            TSNode obj = Field(n, "object");
            if (Is(obj, "self")) {
                singleton = true;
            } else if (Is(obj, "constant") || Is(obj, "scope_resolution")) {
                owner = Join(ctx.owner, Txt(obj));
                if (LastSeg(ctx.owner) == Txt(obj)) owner = ctx.owner;
                singleton = true;
            } else {
                return;  // `def obj.meth` on a local: not part of any class
            }
        }
        auto d = std::make_unique<Def>();
        d->kind = DefKind::Method;
        TSNode name = Field(n, "name");
        d->name = Txt(name);
        d->owner = owner;
        d->singleton = singleton;
        d->params = Params(Field(n, "parameters"));
        d->nesting = ctx.nesting;
        d->visibility = visibility_override.empty() ? st.visibility : visibility_override;
        if (d->name == "initialize") d->visibility = "private";
        d->doc = DocAbove(ts_node_start_point(n).row);
        d->range = RangeOf(n);
        d->sel = RangeOf(name);
        if (st.module_function && !singleton_def && !ctx.singleton) {
            auto copy = std::make_unique<Def>(*d);
            copy->singleton = true;
            copy->visibility.clear();
            Add(std::move(copy));
            d->visibility = "private";
        }
        Add(std::move(d));
    }

    std::vector<std::string> SymbolArgs(TSNode args) const {
        std::vector<std::string> names;
        if (ts_node_is_null(args)) return names;
        const uint32_t n = ts_node_named_child_count(args);
        for (uint32_t i = 0; i < n; i++) {
            TSNode a = ts_node_named_child(args, i);
            std::string s = Txt(a);
            if (Is(a, "simple_symbol") && s.size() > 1) {
                names.push_back(s.substr(1));
            } else if ((Is(a, "string") || Is(a, "delimited_symbol")) && ts_node_named_child_count(a) <= 1) {
                TSNode content = ts_node_named_child(a, 0);
                if (Is(content, "string_content")) names.push_back(Txt(content));
            }
        }
        return names;
    }

    // A statement in a module/class body (or the program).
    void Statement(TSNode n, Ctx &ctx, BodyState &st) {
        const std::string_view ty = NodeType(n);
        if (ty == "module" || ty == "class") {
            TSNode name = Field(n, "name");
            if (ts_node_is_null(name)) return;
            auto d = std::make_unique<Def>();
            d->kind = ty == "module" ? DefKind::Module : DefKind::Class;
            d->fqn = Join(ctx.owner, Txt(name));
            d->name = LastSeg(d->fqn);
            d->owner = Parent(d->fqn);
            d->nesting = ctx.nesting;
            if (ty == "class") {
                TSNode sup = Field(n, "superclass");
                if (!ts_node_is_null(sup) && ts_node_named_child_count(sup) > 0) {
                    d->superclass = Txt(ts_node_named_child(sup, 0));
                }
            }
            d->doc = DocAbove(ts_node_start_point(n).row);
            d->range = RangeOf(n);
            d->sel = RangeOf(name);
            Ctx inner;
            inner.owner = d->fqn;
            inner.nesting = ctx.nesting;
            inner.nesting.push_back(d->fqn);
            inner.ns = Add(std::move(d));
            Body(Field(n, "body"), inner);
            return;
        }
        if (ty == "singleton_class") {
            if (!Is(Field(n, "value"), "self")) return;
            Ctx inner = ctx;
            inner.singleton = true;
            Body(Field(n, "body"), inner);
            return;
        }
        if (ty == "method" || ty == "singleton_method") {
            Method(n, ctx, st, std::string());
            return;
        }
        if (ty == "alias") {
            TSNode a = Field(n, "name"), b = Field(n, "alias");
            auto d = std::make_unique<Def>();
            d->kind = DefKind::Alias;
            d->name = Txt(a);
            d->alias_of = Txt(b);
            if (!d->name.empty() && d->name[0] == ':') d->name.erase(0, 1);
            if (!d->alias_of.empty() && d->alias_of[0] == ':') d->alias_of.erase(0, 1);
            d->owner = ctx.owner;
            d->singleton = ctx.singleton;
            d->nesting = ctx.nesting;
            d->doc = DocAbove(ts_node_start_point(n).row);
            d->range = RangeOf(n);
            d->sel = RangeOf(a);
            Add(std::move(d));
            return;
        }
        if (ty == "assignment") {
            TSNode left = Field(n, "left"), right = Field(n, "right");
            if (!Is(left, "constant") && !Is(left, "scope_resolution")) return;
            // `Point = Struct.new(:x, :y) do ... end`: a class with those attributes.
            if (Is(right, "call")) {
                const std::string recv = Txt(Field(right, "receiver"));
                const std::string meth = Txt(Field(right, "method"));
                if ((recv == "Struct" && meth == "new") || (recv == "Data" && meth == "define") ||
                    (recv == "Class" && meth == "new") || (recv == "Module" && meth == "new")) {
                    auto d = std::make_unique<Def>();
                    d->kind = recv == "Module" ? DefKind::Module : DefKind::Class;
                    d->fqn = Join(ctx.owner, Txt(left));
                    d->name = LastSeg(d->fqn);
                    d->owner = Parent(d->fqn);
                    d->nesting = ctx.nesting;
                    TSNode args = Field(right, "arguments");
                    if (recv == "Class" && !ts_node_is_null(args) && ts_node_named_child_count(args) > 0) {
                        d->superclass = Txt(ts_node_named_child(args, 0));
                    } else if (recv == "Struct") {
                        d->superclass = "Struct";
                    }
                    d->doc = DocAbove(ts_node_start_point(n).row);
                    d->range = RangeOf(n);
                    d->sel = RangeOf(left);
                    const std::string fqn = d->fqn;
                    Ctx inner;
                    inner.owner = fqn;
                    inner.nesting = ctx.nesting;
                    inner.nesting.push_back(fqn);
                    inner.ns = Add(std::move(d));
                    if (recv == "Struct" || recv == "Data") {
                        for (const std::string &a : SymbolArgs(args)) {
                            auto at = std::make_unique<Def>();
                            at->kind = DefKind::Attr;
                            at->attr = recv == "Struct" ? "rw" : "r";
                            at->name = a;
                            at->owner = fqn;
                            at->nesting = inner.nesting;
                            at->range = RangeOf(args);
                            at->sel = RangeOf(args);
                            Add(std::move(at));
                        }
                    }
                    TSNode block = Field(right, "block");
                    if (!ts_node_is_null(block)) Body(Field(block, "body"), inner);
                    return;
                }
            }
            auto d = std::make_unique<Def>();
            d->kind = DefKind::Constant;
            const std::string fqn = Join(ctx.owner, Txt(left));
            d->name = LastSeg(fqn);
            d->owner = Parent(fqn);
            d->fqn = fqn;
            d->nesting = ctx.nesting;
            std::string v = Txt(right);
            const size_t nl = v.find('\n');
            if (nl != std::string::npos) v = v.substr(0, nl) + " ...";
            if (v.size() > 80) v = v.substr(0, 77) + "...";
            d->value = v;
            d->doc = DocAbove(ts_node_start_point(n).row);
            d->range = RangeOf(n);
            d->sel = RangeOf(left);
            Add(std::move(d));
            return;
        }
        if (ty == "identifier") {
            // A bare `private` / `module_function` is an identifier, not a call.
            const std::string word = Txt(n);
            if (word == "module_function") {
                st.module_function = true;
            } else if (word == "private" || word == "protected" || word == "public") {
                st.visibility = word == "public" ? std::string() : word;
                st.module_function = false;
            }
            return;
        }
        if (ty == "call") {
            TSNode recv = Field(n, "receiver");
            const std::string meth = Txt(Field(n, "method"));
            TSNode args = Field(n, "arguments");
            if (!ts_node_is_null(recv)) {
                // `Foo.class_eval do ... end` and friends are left out.
                return;
            }
            if (meth == "attr_accessor" || meth == "attr_reader" || meth == "attr_writer" || meth == "attr") {
                auto doc = DocAbove(ts_node_start_point(n).row);
                for (const std::string &a : SymbolArgs(args)) {
                    auto d = std::make_unique<Def>();
                    d->kind = DefKind::Attr;
                    d->attr = meth == "attr_accessor" ? "rw" : meth == "attr_writer" ? "w" : "r";
                    d->name = a;
                    d->owner = ctx.owner;
                    d->singleton = ctx.singleton;
                    d->nesting = ctx.nesting;
                    d->visibility = st.visibility;
                    d->doc = doc;
                    d->range = RangeOf(n);
                    d->sel = RangeOf(n);
                    // The symbol itself, for definition.
                    const uint32_t k = ts_node_named_child_count(args);
                    for (uint32_t i = 0; i < k; i++) {
                        TSNode s = ts_node_named_child(args, i);
                        if (Txt(s) == ":" + a) d->sel = RangeOf(s);
                    }
                    Add(std::move(d));
                }
                return;
            }
            if ((meth == "include" || meth == "extend" || meth == "prepend") && ctx.ns) {
                const uint32_t k = ts_node_is_null(args) ? 0 : ts_node_named_child_count(args);
                for (uint32_t i = 0; i < k; i++) {
                    TSNode a = ts_node_named_child(args, i);
                    const std::string name = Is(a, "self") ? "self" : Txt(a);
                    if (!(Is(a, "constant") || Is(a, "scope_resolution") || Is(a, "self"))) continue;
                    (meth == "extend" ? ctx.ns->extends : ctx.ns->includes).push_back(name);
                }
                return;
            }
            if (meth == "private" || meth == "protected" || meth == "public" || meth == "module_function" ||
                meth == "private_class_method") {
                const uint32_t k = ts_node_is_null(args) ? 0 : ts_node_named_child_count(args);
                if (k == 0) {
                    if (meth == "module_function") {
                        st.module_function = true;
                    } else if (meth != "private_class_method") {
                        st.visibility = meth == "public" ? std::string() : meth;
                        st.module_function = false;
                    }
                    return;
                }
                for (uint32_t i = 0; i < k; i++) {
                    TSNode a = ts_node_named_child(args, i);
                    if (Is(a, "method") || Is(a, "singleton_method")) {
                        BodyState s2 = st;
                        if (meth == "module_function") s2.module_function = true;
                        Method(a, ctx, s2, meth == "public" || meth == "module_function" ? std::string() : meth);
                        continue;
                    }
                    // `private :a, :b`: mark what is already there.
                    for (const std::string &name : SymbolArgs(args)) {
                        for (auto &d : out->defs) {
                            if (d->owner == ctx.owner && d->name == name && d->kind != DefKind::Module &&
                                d->kind != DefKind::Class && d->kind != DefKind::Constant) {
                                if (meth == "module_function") {
                                    if (!d->singleton) {
                                        auto copy = std::make_unique<Def>(*d);
                                        copy->singleton = true;
                                        out->defs.push_back(std::move(copy));
                                        break;
                                    }
                                } else {
                                    d->visibility = meth == "public" ? std::string() : meth;
                                }
                            }
                        }
                    }
                    break;
                }
                return;
            }
            if (meth == "alias_method") {
                const std::vector<std::string> names = SymbolArgs(args);
                if (names.size() == 2) {
                    auto d = std::make_unique<Def>();
                    d->kind = DefKind::Alias;
                    d->name = names[0];
                    d->alias_of = names[1];
                    d->owner = ctx.owner;
                    d->singleton = ctx.singleton;
                    d->nesting = ctx.nesting;
                    d->doc = DocAbove(ts_node_start_point(n).row);
                    d->range = RangeOf(n);
                    d->sel = RangeOf(n);
                    Add(std::move(d));
                }
                return;
            }
            if (meth == "define_method") {
                const std::vector<std::string> names = SymbolArgs(args);
                if (!names.empty()) {
                    auto d = std::make_unique<Def>();
                    d->kind = DefKind::Method;
                    d->name = names[0];
                    d->owner = ctx.owner;
                    d->singleton = ctx.singleton;
                    d->nesting = ctx.nesting;
                    d->doc = DocAbove(ts_node_start_point(n).row);
                    d->range = RangeOf(n);
                    d->sel = RangeOf(n);
                    TSNode block = Field(n, "block");
                    TSNode bp = ts_node_is_null(block) ? block : Field(block, "parameters");
                    if (!ts_node_is_null(bp)) d->params = Params(bp);
                    Add(std::move(d));
                }
                return;
            }
            // Other calls with a block (`included do`, `class_methods do`, ...) may define methods.
            TSNode block = Field(n, "block");
            if (!ts_node_is_null(block) && (meth == "included" || meth == "extended" || meth == "class_methods" ||
                                            meth == "refine" || meth == "concerning")) {
                Ctx inner = ctx;
                if (meth == "class_methods") inner.singleton = true;
                Body(Field(block, "body"), inner);
            }
            return;
        }
        // Wrappers that may hold definitions: `if defined?(X) ... end`, `begin ... end`.
        if (ty == "if" || ty == "unless" || ty == "begin" || ty == "then" || ty == "else" || ty == "body_statement" ||
            ty == "parenthesized_statements") {
            const uint32_t k = ts_node_named_child_count(n);
            for (uint32_t i = 0; i < k; i++) Statement(ts_node_named_child(n, i), ctx, st);
        }
    }

    void Body(TSNode body, Ctx &ctx) {
        if (ts_node_is_null(body)) return;
        BodyState st;
        const uint32_t k = ts_node_named_child_count(body);
        for (uint32_t i = 0; i < k; i++) Statement(ts_node_named_child(body, i), ctx, st);
    }

    // Requires and $LOAD_PATH additions anywhere in the file.
    void Requires(TSNode n) {
        if (Is(n, "call") && ts_node_is_null(Field(n, "receiver"))) {
            const std::string meth = Txt(Field(n, "method"));
            if (meth == "require" || meth == "require_relative" || meth == "load" || meth == "autoload") {
                TSNode args = Field(n, "arguments");
                if (!ts_node_is_null(args) && ts_node_named_child_count(args) >= 1) {
                    TSNode s = ts_node_named_child(args, meth == "autoload" ? 1 : 0);
                    if (Is(s, "string") && ts_node_named_child_count(s) == 1 &&
                        Is(ts_node_named_child(s, 0), "string_content")) {
                        Require r;
                        r.name = Txt(ts_node_named_child(s, 0));
                        r.relative = meth == "require_relative";
                        r.range = RangeOf(s);
                        for (TSNode p = ts_node_parent(n); !ts_node_is_null(p); p = ts_node_parent(p)) {
                            const std::string_view pt = NodeType(p);
                            if (pt == "program") break;
                            if (pt != "body_statement" && pt != "parenthesized_statements") r.conditional = true;
                        }
                        if (meth == "load" && r.name.size() > 3 && r.name.substr(r.name.size() - 3) == ".rb") {
                            r.relative = false;
                        }
                        out->reqs.push_back(r);
                    }
                }
            }
        }
        if (Is(n, "call") && !ts_node_is_null(Field(n, "receiver"))) {
            const std::string recv = Txt(Field(n, "receiver"));
            const std::string meth = Txt(Field(n, "method"));
            if ((recv == "$LOAD_PATH" || recv == "$:") && (meth == "unshift" || meth == "push" || meth == "<<" ||
                                                          meth == "prepend" || meth == "append")) {
                TSNode args = Field(n, "arguments");
                if (!ts_node_is_null(args) && ts_node_named_child_count(args) >= 1) {
                    const std::string dir = EvalPath(ts_node_named_child(args, 0));
                    if (!dir.empty()) out->load_path.push_back(dir);
                }
            }
        }
        if (Is(n, "binary")) {
            // `$LOAD_PATH << dir`
            TSNode l = Field(n, "left");
            const std::string op = Txt(Field(n, "operator"));
            if ((Txt(l) == "$LOAD_PATH" || Txt(l) == "$:") && op == "<<") {
                const std::string dir = EvalPath(Field(n, "right"));
                if (!dir.empty()) out->load_path.push_back(dir);
            }
        }
        const uint32_t k = ts_node_child_count(n);
        for (uint32_t i = 0; i < k; i++) Requires(ts_node_child(n, i));
    }

    std::string FileDir() const {
        if (out->path.empty()) return std::string();
        return fs::path(out->path).parent_path().string();
    }

    // The value of a path expression: a string literal, `__dir__`, `File.expand_path(x, base)`,
    // `File.join(a, b, ...)`, `File.dirname(__FILE__)`; "" when it is anything else.
    std::string EvalPath(TSNode n) const {
        if (Is(n, "parenthesized_statements") && ts_node_named_child_count(n) == 1) {
            return EvalPath(ts_node_named_child(n, 0));
        }
        if (Is(n, "string")) {
            if (ts_node_named_child_count(n) == 0) return std::string();
            if (ts_node_named_child_count(n) != 1 || !Is(ts_node_named_child(n, 0), "string_content")) {
                return std::string();
            }
            return Txt(ts_node_named_child(n, 0));
        }
        if (Is(n, "file")) return out->path;
        if (Is(n, "identifier") && Txt(n) == "__dir__") return FileDir();
        if (Is(n, "call")) {
            const std::string recv = Txt(Field(n, "receiver"));
            const std::string meth = Txt(Field(n, "method"));
            TSNode args = Field(n, "arguments");
            const uint32_t k = ts_node_is_null(args) ? 0 : ts_node_named_child_count(args);
            if (recv.empty() && meth == "__dir__") return FileDir();
            if (recv != "File") return std::string();
            std::vector<std::string> a;
            for (uint32_t i = 0; i < k; i++) {
                const std::string v = EvalPath(ts_node_named_child(args, i));
                if (v.empty()) return std::string();
                a.push_back(v);
            }
            if (meth == "dirname" && a.size() == 1) return fs::path(a[0]).parent_path().string();
            if (meth == "join" && !a.empty()) {
                fs::path p(a[0]);
                for (size_t i = 1; i < a.size(); i++) p /= a[i];
                return p.lexically_normal().string();
            }
            if ((meth == "expand_path" || meth == "absolute_path") && (a.size() == 1 || a.size() == 2)) {
                fs::path base = a.size() == 2 ? fs::path(a[1]) : fs::path(FileDir());
                fs::path p = fs::path(a[0]).is_absolute() ? fs::path(a[0]) : base / a[0];
                return p.lexically_normal().string();
            }
        }
        return std::string();
    }

    // `# CMD: ruby -I lib -Iext x.rb` in the first lines: its `-I` directories.
    void CmdHeader() {
        for (size_t i = 0; i < lines.size() && i < 15; i++) {
            const std::string &l = lines[i];
            const size_t cmd = l.find("CMD:");
            if (l.find('#') == std::string::npos || cmd == std::string::npos) continue;
            std::istringstream in(l.substr(cmd + 4));
            std::string w;
            bool next = false;
            while (in >> w) {
                if (next) {
                    out->load_path.push_back(w);
                    next = false;
                } else if (w == "-I") {
                    next = true;
                } else if (w.compare(0, 2, "-I") == 0) {
                    out->load_path.push_back(w.substr(2));
                }
            }
        }
        // Relative directories: against the file's directory.
        for (std::string &d : out->load_path) {
            if (!d.empty() && d[0] == '~') {
                if (const char *home = std::getenv("HOME")) d = std::string(home) + d.substr(1);
            }
            if (!fs::path(d).is_absolute() && !out->path.empty()) {
                d = (fs::path(FileDir()) / d).lexically_normal().string();
            }
        }
    }
};

}  // namespace

std::shared_ptr<FileIndex> IndexTree(const Tree &t, const std::string &path) {
    Indexer ix(t, path);
    TSNode root = t.Root();
    ix.MarkComments(root);
    Indexer::Ctx ctx;
    Indexer::BodyState st;
    const uint32_t k = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < k; i++) ix.Statement(ts_node_named_child(root, i), ctx, st);
    ix.Requires(root);
    ix.CmdHeader();
    return ix.out;
}

// ---------------------------------------------------------------------------
// Files on disk

namespace {

struct CacheEntry {
    int64_t mtime = 0;
    int64_t size = 0;
    std::shared_ptr<const FileIndex> index;
};

std::mutex &CacheMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, CacheEntry> &Cache() {
    static std::map<std::string, CacheEntry> c;
    return c;
}

bool IsFile(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool IsDir(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string g_workspace_root;
bool g_override_set = false;
std::vector<std::string> g_override;

}  // namespace

std::shared_ptr<const FileIndex> IndexFile(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return nullptr;
    if (st.st_size > 64 * 1024 * 1024) return nullptr;
    {
        std::lock_guard<std::mutex> lock(CacheMutex());
        auto it = Cache().find(path);
        if (it != Cache().end() && it->second.mtime == static_cast<int64_t>(st.st_mtime) &&
            it->second.size == static_cast<int64_t>(st.st_size)) {
            return it->second.index;
        }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto tree = Parse(ss.str());
    std::shared_ptr<const FileIndex> idx = IndexTree(*tree, path);
    std::lock_guard<std::mutex> lock(CacheMutex());
    CacheEntry &e = Cache()[path];
    e.mtime = static_cast<int64_t>(st.st_mtime);
    e.size = static_cast<int64_t>(st.st_size);
    e.index = idx;
    return idx;
}

void SetWorkspaceRoot(const std::string &root) { g_workspace_root = root; }

void SetLoadPathOverride(const std::vector<std::string> *dirs) {
    g_override_set = dirs != nullptr;
    g_override = dirs ? *dirs : std::vector<std::string>();
    std::lock_guard<std::mutex> lock(CacheMutex());
    Cache().clear();
}

std::vector<std::string> LoadPath(const std::string &path, const std::vector<std::string> &extra) {
    std::vector<std::string> out;
    auto add = [&out](const std::string &d) {
        if (d.empty()) return;
        std::string n = fs::path(d).lexically_normal().string();
        while (n.size() > 1 && n.back() == '/') n.pop_back();
        if (std::find(out.begin(), out.end(), n) == out.end() && IsDir(n)) out.push_back(n);
    };
    for (const std::string &d : extra) add(d);
    if (g_override_set) {
        for (const std::string &d : g_override) add(d);
    } else if (const char *rubylib = std::getenv("RUBYLIB")) {
        std::string s = rubylib;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t colon = s.find(':', start);
            add(s.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    // Gem-like layouts: the `lib/` of the file's ancestors (a script in `examples/` of a gem).
    if (!path.empty()) {
        fs::path dir = fs::path(path).parent_path();
        for (int depth = 0; depth < 8 && !dir.empty(); depth++) {
            if (dir.filename() == "lib") add(dir.string());
            add((dir / "lib").string());
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    // The workspace's `lib/` directories, two levels deep (`ruby/lib`, `bindings/ruby/lib`).
    if (!g_workspace_root.empty() && IsDir(g_workspace_root)) {
        add((fs::path(g_workspace_root) / "lib").string());
        std::error_code ec;
        std::vector<std::string> subs;
        for (fs::directory_iterator it(g_workspace_root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.' || name == "node_modules" || name == "build" || name == "vendor") continue;
            subs.push_back(it->path().string());
        }
        std::sort(subs.begin(), subs.end());
        for (const std::string &s : subs) {
            add((fs::path(s) / "lib").string());
            std::error_code ec2;
            std::vector<std::string> subs2;
            for (fs::directory_iterator it(s, ec2), end; !ec2 && it != end; it.increment(ec2)) {
                if (!it->is_directory(ec2)) continue;
                const std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.') continue;
                subs2.push_back((it->path() / "lib").string());
            }
            std::sort(subs2.begin(), subs2.end());
            for (const std::string &s2 : subs2) add(s2);
        }
    }
    // Last: the file's own directory (a stub next to the script, run with `-I .`).
    if (!path.empty()) add(fs::path(path).parent_path().string());
    return out;
}

namespace {

// Installed gems: `<gem dir>/gems/<name>-<version>/lib`, for the gem whose name is the
// require's first segment.
std::string FindInGems(const std::string &name) {
    std::vector<std::string> roots;
    for (const char *env : {"GEM_HOME", "GEM_PATH"}) {
        const char *v = std::getenv(env);
        if (!v) continue;
        std::string s = v;
        size_t start = 0;
        while (start <= s.size()) {
            const size_t colon = s.find(':', start);
            roots.push_back(s.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
            if (colon == std::string::npos) break;
            start = colon + 1;
        }
    }
    if (roots.empty()) return std::string();
    const std::string gem = name.substr(0, name.find('/'));
    for (const std::string &r : roots) {
        const fs::path gems = fs::path(r) / "gems";
        std::error_code ec;
        std::vector<fs::path> cands;
        for (fs::directory_iterator it(gems, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string fn = it->path().filename().string();
            if (fn.compare(0, gem.size() + 1, gem + "-") == 0 && fn.size() > gem.size() + 1 &&
                std::isdigit(static_cast<unsigned char>(fn[gem.size() + 1]))) {
                cands.push_back(it->path());
            }
        }
        std::sort(cands.begin(), cands.end());
        for (auto it = cands.rbegin(); it != cands.rend(); ++it) {
            const std::string f = (*it / "lib" / (name + ".rb")).string();
            if (IsFile(f)) return f;
        }
    }
    return std::string();
}

}  // namespace

std::string ResolveRequire(const Require &r, const std::string &from_file, const std::vector<std::string> &extra) {
    if (r.name.empty()) return std::string();
    std::string name = r.name;
    const bool has_ext = name.size() > 3 && name.substr(name.size() - 3) == ".rb";
    auto try_file = [&](const fs::path &base) -> std::string {
        const std::string f = (base / (has_ext ? name : name + ".rb")).lexically_normal().string();
        return IsFile(f) ? f : std::string();
    };
    if (r.relative || (name.size() > 1 && (name.compare(0, 2, "./") == 0 || name.compare(0, 3, "../") == 0))) {
        if (from_file.empty()) return std::string();
        return try_file(fs::path(from_file).parent_path());
    }
    if (fs::path(name).is_absolute()) {
        const std::string f = has_ext ? name : name + ".rb";
        return IsFile(f) ? f : std::string();
    }
    for (const std::string &dir : LoadPath(from_file, extra)) {
        const std::string f = try_file(dir);
        if (!f.empty()) return f;
    }
    return FindInGems(name);
}

// ---------------------------------------------------------------------------
// World

void World::Add(std::shared_ptr<const FileIndex> f) {
    if (!f) return;
    if (!f->path.empty() && !paths_.insert(f->path).second) return;
    files_.push_back(f);
    for (const auto &up : f->defs) {
        const Def *d = up.get();
        switch (d->kind) {
        case DefKind::Module:
        case DefKind::Class:
            ns_[d->fqn].push_back(d);
            members_[d->owner].push_back(d);
            break;
        case DefKind::Constant:
            consts_[d->fqn].push_back(d);
            members_[d->owner].push_back(d);
            break;
        case DefKind::Method:
        case DefKind::Attr:
        case DefKind::Alias:
        {
            // Top-level `def`s are (private) methods of Object.
            const std::string owner = d->owner.empty() ? "Object" : d->owner;
            if (d->owner.empty() && !d->singleton && !d->builtin) top_.push_back(d);
            (d->singleton ? smethods_ : methods_)[owner][d->name].push_back(d);
            if (d->kind == DefKind::Attr && d->attr != "r") {
                (d->singleton ? smethods_ : methods_)[owner][d->name + "="].push_back(d);
            }
            break;
        }
        }
    }
}

std::vector<const Def *> World::Namespace(const std::string &fqn) const {
    auto it = ns_.find(fqn);
    return it == ns_.end() ? std::vector<const Def *>() : it->second;
}

std::vector<const Def *> World::Constants(const std::string &fqn) const {
    auto it = consts_.find(fqn);
    return it == consts_.end() ? std::vector<const Def *>() : it->second;
}

std::string World::ResolveConst(const std::string &name, const std::vector<std::string> &nesting) const {
    if (name.empty()) return std::string();
    if (name.compare(0, 2, "::") == 0) {
        const std::string n = name.substr(2);
        return ns_.count(n) || consts_.count(n) ? n : std::string();
    }
    for (size_t i = nesting.size() + 1; i-- > 0;) {
        const std::string cand = i == 0 ? name : nesting[i - 1] + "::" + name;
        if (ns_.count(cand) || consts_.count(cand)) return cand;
    }
    // Constants inherited through the innermost class's ancestors.
    if (!nesting.empty()) {
        for (const std::string &a : Ancestors(nesting.back())) {
            const std::string cand = a + "::" + name;
            if (ns_.count(cand) || consts_.count(cand)) return cand;
        }
    }
    return std::string();
}

void World::Collect(const std::string &fqn, bool singleton, std::set<std::string> &seen,
                    std::vector<std::string> &order) const {
    if (!seen.insert(fqn).second) return;
    order.push_back(fqn);
    const std::vector<const Def *> defs = Namespace(fqn);
    // Included modules, the last included first.
    std::vector<std::pair<std::string, std::vector<std::string>>> incs;
    for (const Def *d : defs) {
        for (const std::string &i : d->includes) incs.emplace_back(i, d->nesting);
    }
    if (!singleton) {
        for (auto it = incs.rbegin(); it != incs.rend(); ++it) {
            std::vector<std::string> nest = it->second;
            nest.push_back(fqn);
            const std::string r = ResolveConst(it->first, nest);
            if (!r.empty()) Collect(r, false, seen, order);
        }
    }
    bool is_class = false;
    std::string sup;
    std::vector<std::string> sup_nest;
    for (const Def *d : defs) {
        if (d->kind == DefKind::Class) is_class = true;
        if (!d->superclass.empty() && sup.empty()) {
            sup = d->superclass;
            sup_nest = d->nesting;
        }
    }
    if (is_class) {
        std::string r = sup.empty() ? std::string() : ResolveConst(sup, sup_nest);
        if (r.empty() && fqn != "Object" && fqn != "BasicObject") r = "Object";
        if (!r.empty()) Collect(r, singleton, seen, order);
    }
}

std::vector<std::string> World::Ancestors(const std::string &fqn) const {
    std::set<std::string> seen;
    std::vector<std::string> order;
    Collect(fqn, false, seen, order);
    return order;
}

namespace {

// The modules @p fqn's singleton responds to through `extend` (`extend self` included).
std::vector<std::string> Extended(const World &w, const std::string &fqn) {
    std::vector<std::string> out;
    for (const Def *d : w.Namespace(fqn)) {
        for (const std::string &e : d->extends) {
            if (e == "self") {
                out.push_back(fqn);
                continue;
            }
            std::vector<std::string> nest = d->nesting;
            nest.push_back(fqn);
            const std::string r = w.ResolveConst(e, nest);
            if (!r.empty()) out.push_back(r);
        }
    }
    return out;
}

bool IsModule(const World &w, const std::string &fqn) {
    const auto defs = w.Namespace(fqn);
    if (defs.empty()) return false;
    for (const Def *d : defs) {
        if (d->kind == DefKind::Class) return false;
    }
    return true;
}

}  // namespace

std::vector<const Def *> World::Methods(const std::string &fqn, bool singleton, const std::string &name) const {
    if (!singleton) {
        const std::vector<std::string> chain = Ancestors(fqn.empty() ? "Object" : fqn);
        for (const std::string &a : chain) {
            auto it = methods_.find(a);
            if (it == methods_.end()) continue;
            auto jt = it->second.find(name);
            if (jt != it->second.end() && !jt->second.empty()) return jt->second;
        }
        return {};
    }
    std::set<std::string> seen;
    std::vector<std::string> chain;
    Collect(fqn, true, seen, chain);
    for (const std::string &a : chain) {
        auto it = smethods_.find(a);
        if (it != smethods_.end()) {
            auto jt = it->second.find(name);
            if (jt != it->second.end() && !jt->second.empty()) return jt->second;
        }
        for (const std::string &e : Extended(*this, a)) {
            for (const std::string &ea : Ancestors(e)) {
                auto mt = methods_.find(ea);
                if (mt == methods_.end()) continue;
                auto jt = mt->second.find(name);
                if (jt != mt->second.end() && !jt->second.empty()) return jt->second;
            }
        }
    }
    // The class object's own methods (Class#new, Module#name, ...).
    for (const std::string &meta : IsModule(*this, fqn) ? Ancestors("Module") : Ancestors("Class")) {
        auto it = methods_.find(meta);
        if (it == methods_.end()) continue;
        auto jt = it->second.find(name);
        if (jt != it->second.end() && !jt->second.empty()) return jt->second;
    }
    return {};
}

std::vector<const Def *> World::AllMethods(const std::string &fqn, bool singleton) const {
    std::vector<const Def *> out;
    std::set<std::string> names;
    auto take = [&](const std::map<std::string, std::vector<const Def *>> &m) {
        for (const auto &kv : m) {
            if (kv.second.empty() || !names.insert(kv.first).second) continue;
            out.push_back(kv.second.front());
        }
    };
    if (!singleton) {
        for (const std::string &a : Ancestors(fqn.empty() ? "Object" : fqn)) {
            auto it = methods_.find(a);
            if (it != methods_.end()) take(it->second);
        }
        return out;
    }
    std::set<std::string> seen;
    std::vector<std::string> chain;
    Collect(fqn, true, seen, chain);
    for (const std::string &a : chain) {
        auto it = smethods_.find(a);
        if (it != smethods_.end()) take(it->second);
        for (const std::string &e : Extended(*this, a)) {
            for (const std::string &ea : Ancestors(e)) {
                auto mt = methods_.find(ea);
                if (mt != methods_.end()) take(mt->second);
            }
        }
    }
    for (const std::string &meta : IsModule(*this, fqn) ? Ancestors("Module") : Ancestors("Class")) {
        auto it = methods_.find(meta);
        if (it != methods_.end()) take(it->second);
    }
    return out;
}

std::vector<const Def *> World::Members(const std::string &fqn) const {
    auto it = members_.find(fqn);
    return it == members_.end() ? std::vector<const Def *>() : it->second;
}

std::vector<const Def *> World::TopLevelMethods() const { return top_; }

}  // namespace rblsp
