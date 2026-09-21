#include "js_engine.h"

#include "url_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#if !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <ucontext.h>
#define MEP_JS_COROUTINES 1
#endif
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "html_doc.h"
#include "json.h"
#include "regex.h"
#include "svg_doc.h"

// A hand-rolled tokenizer + recursive-descent/precedence-climbing parser +
// tree-walking interpreter for a *subset* of JS -- not spec-compliant, not
// aiming to run real-world scripts, only small hand-written ones against a
// page's DOM (see js_engine.h's own header for the exact rationale/use
// case this was scoped to).
//
// Supported: var/let/const (all three are plain mutable bindings in the
// current function/global scope -- no let/const block-scoping or temporal-
// dead-zone semantics, since nothing this is meant to run depends on that
// distinction); function declarations/expressions and arrow functions,
// all with real closures; if/else, while, for(;;), return, break,
// continue, blocks; number/string/boolean/null/undefined/array/object
// literals; template literals (`...${expr}...`); +-*/%, comparisons
// (== and != behave like === and !== -- no ToPrimitive/type-coercion
// ladder, since every value this engine's own DOM bindings hand back is
// already a definite type), && || ! (short-circuiting), unary -/+/typeof,
// = and compound assignment, ?:, member/index access, calls.
//
// Not supported at all: `this`, `new`, prototypes/classes, generators/
// async, destructuring, spread/rest, labeled statements, getters/setters
// on plain objects (only the DOM bindings below have magic properties),
// regex literals, try/catch (a script that would use it just throws
// itself instead -- see RunScripts's own per-script catch-all).
//
// DOM binding surface -- deliberately this small, see js_engine.h:
// document.getElementById(id), document.title (get/set), a DOM-wrapping
// object's .textContent (get/set), console.log(...args), and a bare
// `window` object scripts can assign arbitrary properties onto (no BOM
// methods -- setTimeout/location/etc. aren't implemented) so the common
// "window.Foo = {...}" config-stashing pattern doesn't throw
// ReferenceError.

namespace {

// ---------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------

enum class VType { Undefined, Null, Number, String, Boolean, Object };

struct ObjectData;
using ObjectPtr = std::shared_ptr<ObjectData>;
struct Environment;
using EnvPtr = std::shared_ptr<Environment>;
struct Node;

struct Value {
    VType type = VType::Undefined;
    double num = 0;
    std::string str;
    bool boolean = false;
    ObjectPtr obj;

    /**
     * @brief Constructs a default-initialized value (VType::Undefined).
     * @return An undefined Value.
     */
    static Value Undef() { return Value{}; }
    /**
     * @brief Constructs a value of type Null.
     * @return A Value with type VType::Null.
     */
    static Value MakeNull() {
        Value v;
        v.type = VType::Null;
        return v;
    }
    /**
     * @brief Constructs a numeric value.
     * @param d The numeric payload.
     * @return A Value with type VType::Number holding d.
     */
    static Value Num(double d) {
        Value v;
        v.type = VType::Number;
        v.num = d;
        return v;
    }
    /**
     * @brief Constructs a string value.
     * @param s The string payload, moved into the result.
     * @return A Value with type VType::String holding s.
     */
    static Value Str(std::string s) {
        Value v;
        v.type = VType::String;
        v.str = std::move(s);
        return v;
    }
    /**
     * @brief Constructs a boolean value.
     * @param b The boolean payload.
     * @return A Value with type VType::Boolean holding b.
     */
    static Value Bool(bool b) {
        Value v;
        v.type = VType::Boolean;
        v.boolean = b;
        return v;
    }
    /**
     * @brief Constructs an object value wrapping the given object pointer.
     * @param o The object pointer, moved into the result.
     * @return A Value with type VType::Object holding o.
     */
    static Value Obj(ObjectPtr o) {
        Value v;
        v.type = VType::Object;
        v.obj = std::move(o);
        return v;
    }

    /**
     * @brief Computes this value's JS-style truthiness (ToBoolean).
     * @return false for undefined/null, false for 0/NaN numbers, false for an empty string, and true for any object.
     */
    bool Truthy() const {
        switch (type) {
            case VType::Undefined:
            case VType::Null:
                return false;
            case VType::Boolean:
                return boolean;
            case VType::Number:
                return num != 0 && !std::isnan(num);
            case VType::String:
                return !str.empty();
            case VType::Object:
                return true;
        }
        return false;
    }
};

// A single "kind" field serves plain objects, arrays (numeric string keys
// in `props`, plus a maintained "length"), user-defined functions/arrow
// functions (`fn_node`+`closure`), native/builtin functions (`native`),
// and DOM element wrappers (`dom_node`) -- a real engine would split these
// into a class hierarchy; one struct is simpler here since nothing but
// property get/set (below) ever needs to branch on which kind it is.
using NativeFn = std::function<Value(std::vector<Value> &, bool &threw, std::string &err)>;

// Own properties in insertion order -- what Object.keys, for...in,
// JSON.stringify and spread are specified to observe -- with O(1) lookup.
// Erased slots become tombstones (compacted once they outnumber the live
// entries) so array shift/pop churn stays cheap.
class PropertyMap {
public:
    using Entry = std::pair<std::string, Value>;

    template <bool Const>
    class Iter {
    public:
        using Map = std::conditional_t<Const, const PropertyMap, PropertyMap>;
        using Ref = std::conditional_t<Const, const Entry &, Entry &>;
        using Ptr = std::conditional_t<Const, const Entry *, Entry *>;
        Iter(Map *map, size_t at) : map_(map), at_(at) { Skip(); }
        Ref operator*() const { return map_->entries_[at_]; }
        Ptr operator->() const { return &map_->entries_[at_]; }
        Iter &operator++() { ++at_; Skip(); return *this; }
        bool operator==(const Iter &other) const { return at_ == other.at_; }
        bool operator!=(const Iter &other) const { return at_ != other.at_; }
        size_t Index() const { return at_; }

    private:
        void Skip() { while (at_ < map_->entries_.size() && !map_->alive_[at_]) ++at_; }
        Map *map_;
        size_t at_;
    };
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, entries_.size()); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, entries_.size()); }

    Value &operator[](const std::string &key) {
        auto found = index_.find(key);
        if (found != index_.end()) return entries_[found->second].second;
        index_[key] = entries_.size();
        entries_.emplace_back(key, Value());
        alive_.push_back(true);
        return entries_.back().second;
    }
    iterator find(const std::string &key) {
        auto found = index_.find(key);
        return found == index_.end() ? end() : iterator(this, found->second);
    }
    const_iterator find(const std::string &key) const {
        auto found = index_.find(key);
        return found == index_.end() ? end() : const_iterator(this, found->second);
    }
    size_t count(const std::string &key) const { return index_.count(key); }
    bool contains(const std::string &key) const { return index_.count(key) != 0; }
    size_t size() const { return index_.size(); }
    bool empty() const { return index_.empty(); }
    void clear() { entries_.clear(); alive_.clear(); index_.clear(); }
    size_t erase(const std::string &key) {
        auto found = index_.find(key);
        if (found == index_.end()) return 0;
        alive_[found->second] = false;
        entries_[found->second].second = Value();
        index_.erase(found);
        if (entries_.size() > 32 && index_.size() * 2 < entries_.size()) Compact();
        return 1;
    }

private:
    void Compact() {
        std::deque<Entry> kept;
        for (size_t i = 0; i < entries_.size(); ++i) if (alive_[i]) kept.push_back(std::move(entries_[i]));
        entries_ = std::move(kept);
        alive_.assign(entries_.size(), true);
        index_.clear();
        for (size_t i = 0; i < entries_.size(); ++i) index_[entries_[i].first] = i;
    }
    std::deque<Entry> entries_;  // push_back keeps references valid (props[a] = props[b])
    std::vector<bool> alive_;
    std::unordered_map<std::string, size_t> index_;
};

struct ObjectData {
    PropertyMap props;
    std::unordered_map<std::string, ObjectPtr> getters;
    std::unordered_map<std::string, ObjectPtr> setters;
    std::vector<std::string> private_field_names;
    ObjectPtr prototype;
    bool is_array = false;
    bool is_function = false;
    bool is_generator_function = false;
    bool is_async_function = false;
    bool is_arrow_function = false;  // no own `this`/`arguments`/`prototype`
    bool is_date = false;            // a Date; date_ms is its time value
    double date_ms = 0;
    bool is_regexp = false;
    bool is_promise = false;
    // 0 pending, 1 fulfilled, 2 rejected. Reactions retain their paired
    // fulfillment/rejection handlers and downstream promise.
    int promise_state = 0;
    Value promise_value;
    std::vector<std::tuple<ObjectPtr, ObjectPtr, ObjectPtr>> promise_reactions;
    std::shared_ptr<mep_regex::Regex> regexp;
    std::vector<std::string> regexp_group_names;  // index = capture group number; "" for unnamed
    bool regexp_global = false;
    long regexp_last_index = 0;
    bool is_symbol = false;
    bool is_location = false;
    bool is_window = false;
    ObjectPtr location_object;
    ObjectPtr proxy_target;
    ObjectPtr proxy_handler;
    bool is_map = false;
    bool is_set = false;
    std::vector<std::pair<Value, Value>> collection_entries;
    bool is_class = false;
    bool frozen = false;
    ObjectPtr super_class;  // set on methods declared by an extends class
    bool is_document = false;

    const Node *fn_node = nullptr;  // function/arrow AST node (params+body); owned by the Program this ran from, so this stays valid for RunScripts's own duration
    const Node *class_node = nullptr;
    EnvPtr closure;

    NativeFn native;

    DomNode *dom_node = nullptr;
    DomNode *style_node = nullptr;  // non-null only for element.style wrappers
    DomNode *dataset_node = nullptr;  // non-null only for element.dataset wrappers
    std::string symbol_key;           // a Symbol's unique property key (see PropertyKey)
    std::unordered_set<std::string> non_enumerable;  // own keys defineProperty hid from enumeration
    bool is_list_iterator = false;    // MakeListIterator: its own [Symbol.iterator]() is itself
    HtmlDoc *owner_doc = nullptr;  // only set on the `document` object, for .title
    bool is_canvas_context = false;
    DomNode *canvas_node = nullptr;
    unsigned char canvas_r = 0, canvas_g = 0, canvas_b = 0, canvas_a = 255;  // fillStyle
    unsigned char canvas_stroke_r = 0, canvas_stroke_g = 0, canvas_stroke_b = 0, canvas_stroke_a = 255;  // strokeStyle
    float canvas_line_width = 1.0f;
    float canvas_font_size = 16.0f;
    float canvas_global_alpha = 1.0f;
    float canvas_transform[6] = {1, 0, 0, 1, 0, 0};  // CTM: a b c d e f
    // A fillStyle/strokeStyle assigned a CanvasGradient object rather than
    // a color string; null when the flat color above is in effect.
    std::shared_ptr<ObjectData> canvas_fill_gradient, canvas_stroke_gradient;
    // The current default path, one flat x,y list per subpath, in
    // already-transformed canvas coordinates.
    std::vector<std::vector<float>> canvas_subpaths;
    std::vector<std::vector<float>> canvas_state_stack;  // save(): flattened numeric state
    std::vector<std::pair<std::shared_ptr<ObjectData>, std::shared_ptr<ObjectData>>> canvas_gradient_stack;
    // CanvasGradient objects (createLinearGradient/createRadialGradient).
    bool is_canvas_gradient = false;
    CanvasGradient gradient;
};

Value WrapDomNode(HtmlDoc &doc, DomNode *node);
Value MakeNativeFn(NativeFn fn);
void SetDomEventHandler(const ObjectPtr &obj, const std::string &key, const Value &value);

struct Environment {
    std::unordered_map<std::string, Value> vars;
    EnvPtr parent;
    // A function's (or the program's) own scope -- where `var` declarations
    // land, however deeply nested the block that contains them.
    bool is_function_scope = false;

    /**
     * @brief The nearest enclosing function/program scope (this one included).
     * @return That scope; the outermost scope when none is marked.
     */
    Environment *FunctionScope() {
        Environment *e = this;
        while (!e->is_function_scope && e->parent) e = e->parent.get();
        return e;
    }

    /**
     * @brief Looks up a binding by name, walking outward through parent scopes.
     * @param name The identifier to look up.
     * @return A pointer to the binding's Value if found in this scope or an ancestor, else nullptr.
     */
    Value *Find(const std::string &name) {
        for (Environment *e = this; e != nullptr; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
            if (!e->aliases.empty()) {
                auto alias = e->aliases.find(name);
                if (alias != e->aliases.end()) return alias->second.first->FindOwn(alias->second.second);
            }
        }
        return nullptr;
    }
    // An imported name is not a copy: it reads (and `counter++` in the
    // exporter updates) the exporting module's own binding.
    std::unordered_map<std::string, std::pair<EnvPtr, std::string>> aliases;
    Value *FindOwn(const std::string &name, int depth = 0) {
        auto it = vars.find(name);
        if (it != vars.end()) return &it->second;
        auto alias = aliases.find(name);
        if (alias != aliases.end() && depth < 32) return alias->second.first->FindOwn(alias->second.second, depth + 1);
        return nullptr;
    }
    /**
     * @brief Creates or overwrites a binding in this scope (not any ancestor).
     * @param name The identifier to bind.
     * @param v The value to bind it to.
     */
    void Define(const std::string &name, Value v) { vars[name] = std::move(v); }
};

/**
 * @brief Converts a JS number to its display string, matching JS's own Number-to-String rules for the common cases.
 * @param d The number to convert.
 * @return "NaN"/"Infinity"/"-Infinity" for those special values, an integer literal for whole numbers under 1e15 in magnitude, otherwise a 15-significant-digit decimal rendering.
 */
std::string NumberToString(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    // JS ToString renders -0 as "0" too (no sign), unlike most other
    // negative numbers -- this isn't a sign-handling bug, both branches
    // are deliberately the same string.
    if (d == 0) return "0";
    if (std::fabs(d) < 9e18 && d == static_cast<double>(static_cast<long long>(d))) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        return buf;
    }
    std::ostringstream oss;
    for (int precision = 15; precision <= 17; ++precision) {
        oss.str("");
        oss.precision(precision);
        oss << d;
        if (std::strtod(oss.str().c_str(), nullptr) == d) break;
    }
    return oss.str();
}

bool ResolveDocumentResource(const std::string &base_dir, const std::string &url, std::filesystem::path &out) {
    if (base_dir.empty() || url.empty() || url.find("://") != std::string::npos) return false;
    std::filesystem::path requested(url);
    if (requested.is_absolute()) return false;
    std::error_code error;
    const std::filesystem::path base = std::filesystem::weakly_canonical(std::filesystem::path(base_dir), error);
    if (error) return false;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(base / requested, error);
    if (error) return false;
    auto base_it = base.begin(), candidate_it = candidate.begin();
    for (; base_it != base.end(); ++base_it, ++candidate_it) if (candidate_it == candidate.end() || *base_it != *candidate_it) return false;
    out = candidate;
    return true;
}

/**
 * @brief Reads an array object's "length" property.
 * @param obj The array object to inspect.
 * @return The numeric value of obj's "length" property, or 0 if it has none.
 */
long ArrayLength(const ObjectPtr &obj) {
    auto it = obj->props.find("length");
    if (it == obj->props.end()) return 0;
    return static_cast<long>(it->second.num);
}

std::string ToDisplayString(const Value &v);
std::string DateToIso(double ms);

/**
 * @brief Joins an array object's elements (index 0..length-1) into a comma-separated display string, matching JS's default Array.toString().
 * @param obj The array object to join.
 * @return The comma-joined display strings of obj's elements, skipping any missing index.
 */
std::string JoinArrayForDisplay(const ObjectPtr &obj) {
    std::string out;
    long len = ArrayLength(obj);
    for (long i = 0; i < len; i++) {
        if (i) out += ",";
        auto it = obj->props.find(std::to_string(i));
        if (it != obj->props.end()) out += ToDisplayString(it->second);
    }
    return out;
}

/**
 * @brief Converts a Value to the string JS's implicit ToString/template-literal coercion would produce.
 * @param v The value to convert.
 * @return "undefined"/"null"/"true"/"false" for those value kinds, the number formatted via NumberToString, the string itself, or an object rendering ("[object HTMLElement]", the joined array, "function", or "[object Object]").
 */
std::string ToDisplayString(const Value &v) {
    switch (v.type) {
        case VType::Undefined:
            return "undefined";
        case VType::Null:
            return "null";
        case VType::Boolean:
            return v.boolean ? "true" : "false";
        case VType::Number:
            return NumberToString(v.num);
        case VType::String:
            return v.str;
        case VType::Object:
            if (!v.obj) return "null";
            if (v.obj->dom_node) return "[object HTMLElement]";
            if (v.obj->is_array) return JoinArrayForDisplay(v.obj);
            if (v.obj->is_function) return "function";
            if (v.obj->is_date) return DateToIso(v.obj->date_ms);
            if (v.obj->is_symbol) { auto d = v.obj->props.find("description"); return "Symbol(" + (d == v.obj->props.end() ? std::string() : ToDisplayString(d->second)) + ")"; }
            if (v.obj->is_regexp) { auto src = v.obj->props.find("source"); return "/" + (src == v.obj->props.end() ? std::string() : ToDisplayString(src->second)) + "/"; }
            // An Error (anywhere on the chain: class X extends Error) reads "Name: message".
            if (v.obj->props.contains("message") && v.obj->props.contains("stack")) {
                Value name = Value::Undef();
                for (ObjectPtr cur = v.obj; cur && name.type == VType::Undefined; cur = cur->prototype) { auto found = cur->props.find("name"); if (found != cur->props.end()) name = found->second; }
                const std::string message = ToDisplayString(v.obj->props.find("message")->second);
                const std::string label = name.type == VType::Undefined ? "Error" : ToDisplayString(name);
                return message.empty() ? label : label + ": " + message;
            }
            return "[object Object]";
    }
    return "";
}

/**
 * @brief Converts a Value to a number, matching JS's ToNumber for the value kinds this engine has.
 * @param v The value to convert.
 * @return 0/1 for booleans, 0 for null, NaN for undefined and for objects, the number itself for numbers, and for strings the parsed number (0 for an empty string, NaN if any non-numeric/non-trailing-whitespace text remains).
 */
double ToNumber(const Value &v) {
    switch (v.type) {
        case VType::Number:
            return v.num;
        case VType::Boolean:
            return v.boolean ? 1 : 0;
        case VType::Null:
            return 0;
        case VType::Undefined:
            return std::nan("");
        case VType::String: {
            if (v.str.empty()) return 0;
            const char *s = v.str.c_str();
            char *end = nullptr;
            double d = std::strtod(s, &end);
            if (end == s) return std::nan("");
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
            return (*end == '\0') ? d : std::nan("");
        }
        case VType::Object:
            if (v.obj && v.obj->is_date) return v.obj->date_ms;
            return std::nan("");
    }
    return std::nan("");
}

/**
 * @brief Tests two values for strict (type-and-value) equality, the semantics this engine's own == and != operators use.
 * @param a The left-hand value.
 * @param b The right-hand value.
 * @return false if a and b have different VTypes; otherwise true for undefined/null (always equal to their own type), and a plain value comparison for boolean/number/string, or pointer identity for objects.
 */
bool StrictEquals(const Value &a, const Value &b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VType::Undefined:
        case VType::Null:
            return true;
        case VType::Boolean:
            return a.boolean == b.boolean;
        case VType::Number:
            return a.num == b.num;
        case VType::String:
            return a.str == b.str;
        case VType::Object:
            return a.obj == b.obj;
    }
    return false;
}

/**
 * @brief Recursively concatenates the text of a DOM node's descendant Text nodes, matching the real DOM's .textContent getter.
 * @param n The DOM node whose descendants' text is concatenated.
 * @return The concatenation of every descendant Text node's text, in document order.
 */
std::string GetTextContent(const DomNode *n) {
    std::string out;
    for (const auto &c : n->children) {
        if (c->type == DomNodeType::Text) {
            out += c->text;
        } else {
            out += GetTextContent(c.get());
        }
    }
    return out;
}

std::string EscapeHtmlText(const std::string &text) {
    std::string escaped;
    for (char c : text) {
        if (c == '&') escaped += "&amp;";
        else if (c == '<') escaped += "&lt;";
        else if (c == '>') escaped += "&gt;";
        else escaped += c;
    }
    return escaped;
}

std::string SerializeDomNode(const DomNode *node) {
    if (!node) return "";
    if (node->type == DomNodeType::Text) return EscapeHtmlText(node->text);
    if (node->tag == "#document") { std::string all; for (const auto &child : node->children) all += SerializeDomNode(child.get()); return all; }
    std::string html = "<" + node->tag;
    for (const auto &[name, value] : node->attrs) if (!name.empty() && name[0] != '\x01') html += " " + name + "=\"" + EscapeHtmlText(value) + "\"";  // \x01-prefixed: engine-private caches
    static const std::unordered_set<std::string> void_tags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    if (void_tags.count(node->tag)) return html + ">";
    html += ">"; for (const auto &child : node->children) html += SerializeDomNode(child.get()); return html + "</" + node->tag + ">";
}

void RetireChildren(DomNode *node);

void ReplaceInnerHtml(DomNode *node, const std::string &html) {
    if (!node || node->type != DomNodeType::Element) return;
    HtmlDoc fragment; ParseHtml(html, fragment);
    RetireChildren(node);
    if (!fragment.root) return;
    for (auto &child : fragment.root->children) { child->parent = node; node->children.push_back(std::move(child)); }
}

// Replaces every child with a single Text node -- matches real DOM's own
// `el.textContent = x` (any existing children, element or text, are gone),
// not an append. Deliberately doesn't call ComputeStyles: a Text node's
// own style is never consulted by anything (js_engine.h's own comment) and
// this doesn't touch the element's tag/attrs/position, so the element's
// *own* already-computed style stays correct.
/**
 * @brief Sets a DOM node's textContent by discarding its existing children and replacing them with a single new Text node.
 * @param n The DOM node whose children are replaced.
 * @param text The text for the new sole Text child.
 */
void SetTextContent(DomNode *n, const std::string &text) {
    RetireChildren(n);
    auto t = std::make_unique<DomNode>();
    t->type = DomNodeType::Text;
    t->text = text;
    t->parent = n;
    n->children.push_back(std::move(t));
}

/**
 * @brief Recursively searches an element subtree (depth-first, pre-order) for an element with the given id attribute.
 * @param n The subtree root to search, including itself.
 * @param id The id value to match.
 * @return A pointer to the first matching element node found, or nullptr if none matches.
 */
DomNode *FindById(DomNode *n, const std::string &id) {
    if (n->type == DomNodeType::Element && n->Id() == id) return n;
    for (auto &c : n->children) {
        if (DomNode *found = FindById(c.get(), id)) return found;
    }
    return nullptr;
}

DomNode *FindByTag(DomNode *n, const std::string &tag) {
    if (!n) return nullptr;
    if (n->type == DomNodeType::Element && n->tag == tag) return n;
    for (auto &child : n->children) if (DomNode *found = FindByTag(child.get(), tag)) return found;
    return nullptr;
}

/**
 * @brief Determines whether a property key string is a non-negative-integer array index (i.e. consists only of digits).
 * @param key The property key to test.
 * @param idx Set to the parsed integer value of key when it is a valid index; left untouched otherwise.
 * @return true if key is non-empty and every character is a digit, false otherwise.
 */
bool IsArrayIndexKey(const std::string &key, long &idx) {
    if (key.empty()) return false;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
    }
    idx = std::strtol(key.c_str(), nullptr, 10);
    return true;
}

/**
 * @brief Reads a property from an object, resolving the magic textContent/title bindings before falling back to plain stored properties.
 * @param obj The object to read from (may be null).
 * @param key The property name to read.
 * @return undefined if obj is null; the DOM element's live text content for a DOM-wrapper's "textContent"; the owning document's title for the document object's "title"; the stored property value if present; undefined otherwise.
 */
// The string a value names a property by. A Symbol gets a key no string
// can collide with ("@@..."), which enumeration then leaves out.
std::string PropertyKey(const Value &v) {
    if (v.type == VType::Number) return NumberToString(v.num);
    if (v.type == VType::Object && v.obj && v.obj->is_symbol) {
        if (v.obj->symbol_key.empty()) {
            static long next_symbol = 0;
            v.obj->symbol_key = "@@sym:" + std::to_string(++next_symbol);
        }
        return v.obj->symbol_key;
    }
    return ToDisplayString(v);
}
bool IsHiddenKey(const std::string &key) { return key.size() > 1 && key[0] == '@' && key[1] == '@'; }

bool IsArrayIndexKey(const std::string &key, long &idx);

// Own enumerable string keys in specification order: array indices
// ascending, then the rest in insertion order. Skips an array's `length`,
// symbol keys and anything defineProperty made non-enumerable.
std::vector<std::string> EnumerableKeys(const ObjectPtr &obj) {
    std::vector<std::pair<long, std::string>> indices;
    std::vector<std::string> names;
    if (!obj) return names;
    for (const auto &entry : obj->props) {
        const std::string &key = entry.first;
        if ((obj->is_array && key == "length") || IsHiddenKey(key) || obj->non_enumerable.count(key)) continue;
        if (obj->is_function && (key == "prototype")) continue;
        long index = 0;
        if (IsArrayIndexKey(key, index)) indices.emplace_back(index, key);
        else names.push_back(key);
    }
    std::sort(indices.begin(), indices.end());
    std::vector<std::string> out;
    out.reserve(indices.size() + names.size());
    for (auto &entry : indices) out.push_back(std::move(entry.second));
    for (auto &name : names) out.push_back(std::move(name));
    return out;
}

Value MakeArray(const std::vector<Value> &values) {
    auto array = std::make_shared<ObjectData>();
    array->is_array = true;
    for (size_t i = 0; i < values.size(); ++i) array->props[std::to_string(i)] = values[i];
    array->props["length"] = Value::Num(static_cast<double>(values.size()));
    return Value::Obj(array);
}

// The `this` a native was called with (natives take only their arguments).
thread_local const Value *g_native_this = nullptr;
Value NativeThis() { return g_native_this ? *g_native_this : Value::Undef(); }

// An iterator object ({next(), [Symbol.iterator]()}) over a fixed list.
Value MakeListIterator(std::vector<Value> values) {
    auto iterator = std::make_shared<ObjectData>();
    auto items = std::make_shared<std::vector<Value>>(std::move(values));
    auto position = std::make_shared<size_t>(0);
    iterator->props["next"] = MakeNativeFn([items, position](const std::vector<Value> &, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>();
        const bool done = *position >= items->size();
        result->props["value"] = done ? Value::Undef() : (*items)[(*position)++];
        result->props["done"] = Value::Bool(done);
        return Value::Obj(result);
    });
    iterator->props["@@iterator"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return NativeThis(); });
    iterator->is_list_iterator = true;
    return Value::Obj(iterator);
}


// Intrinsic pixel size from an image file's header (PNG, GIF, BMP, JPEG,
// WebP/VP8X, SVG width/height) -- enough for img.naturalWidth/Height
// without pulling a decoder into the script engine.
bool SniffImageSize(const std::string &bytes, int &width, int &height) {
    const auto *b = reinterpret_cast<const unsigned char *>(bytes.data());
    const size_t n = bytes.size();
    auto be32 = [&](size_t at) { return static_cast<int>((static_cast<unsigned>(b[at]) << 24) | (static_cast<unsigned>(b[at + 1]) << 16) | (static_cast<unsigned>(b[at + 2]) << 8) | b[at + 3]); };
    auto le16 = [&](size_t at) { return static_cast<int>(b[at] | (b[at + 1] << 8)); };
    auto le32 = [&](size_t at) { return static_cast<int>(static_cast<unsigned>(b[at]) | (static_cast<unsigned>(b[at + 1]) << 8) | (static_cast<unsigned>(b[at + 2]) << 16) | (static_cast<unsigned>(b[at + 3]) << 24)); };
    if (n >= 24 && bytes.compare(1, 3, "PNG") == 0) { width = be32(16); height = be32(20); return true; }
    if (n >= 10 && bytes.compare(0, 3, "GIF") == 0) { width = le16(6); height = le16(8); return true; }
    if (n >= 26 && bytes.compare(0, 2, "BM") == 0) { width = le32(18); height = std::abs(le32(22)); return true; }
    if (n >= 30 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0 && bytes.compare(12, 4, "VP8X") == 0) {
        width = 1 + (b[24] | (b[25] << 8) | (b[26] << 16)); height = 1 + (b[27] | (b[28] << 8) | (b[29] << 16)); return true;
    }
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        for (size_t at = 2; at + 9 < n;) {
            if (b[at] != 0xFF) { ++at; continue; }
            const unsigned char marker = b[at + 1];
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                height = (b[at + 5] << 8) | b[at + 6]; width = (b[at + 7] << 8) | b[at + 8]; return true;
            }
            at += 2 + static_cast<size_t>((b[at + 2] << 8) | b[at + 3]);
        }
        return false;
    }
    const size_t svg = bytes.find("<svg");
    if (svg != std::string::npos) {
        auto attr = [&](const char *name) {
            const size_t at = bytes.find(std::string(name) + "=\"", svg);
            return at == std::string::npos ? 0 : std::atoi(bytes.c_str() + at + std::strlen(name) + 2);
        };
        width = attr(" width"); height = attr(" height");
        return width > 0 && height > 0;
    }
    return false;
}

// Loads an <img>'s bytes once (over the page's fetch hook, or from disk for
// a file:// page) and caches the sniffed size on the node.
bool DomImageSize(HtmlDoc &doc, DomNode *node, int &width, int &height) {
    auto cached = node->attrs.find("\x01natural-size");
    auto src = node->attrs.find("src");
    const std::string src_text = src == node->attrs.end() ? "" : src->second;
    if (cached == node->attrs.end() || cached->second.rfind(src_text + "|", 0) != 0) {
        int w = 0, h = 0;
        if (!src_text.empty()) {
            const std::string url = urlutil::ResolveUrl(doc.document_url, src_text);
            std::string bytes;
            if (urlutil::IsHttpUrl(url)) {
                if (const HtmlUrlFetcher &fetch = GetHtmlUrlFetcher()) { HtmlFetchResult got = fetch(url); if (got.error.empty() && got.status >= 200 && got.status < 300) bytes = std::move(got.body); }
            } else if (url.rfind("file://", 0) == 0) {
                std::ifstream in(url.substr(7), std::ios::binary);
                if (in) bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (!SniffImageSize(bytes, w, h)) { w = 0; h = 0; }
        }
        node->attrs["\x01natural-size"] = src_text + "|" + std::to_string(w) + "x" + std::to_string(h);
        cached = node->attrs.find("\x01natural-size");
    }
    const std::string size = cached->second.substr(cached->second.rfind('|') + 1);
    width = std::atoi(size.c_str());
    height = std::atoi(size.c_str() + size.find('x') + 1);
    return width > 0 && height > 0;
}

// ---- <select>/<option> state ------------------------------------------------
// An option's live selectedness is kept in a private attribute ("\x01selected");
// until a script or the user changes it, the `selected` content attribute
// (and "first option of a single select") decides, as in the platform.
void CollectOptions(DomNode *root, std::vector<DomNode *> &out) {
    for (const auto &child : root->children) {
        if (child->tag == "option") out.push_back(child.get());
        else CollectOptions(child.get(), out);
    }
}
std::string OptionValue(DomNode *option) {
    auto attr = option->attrs.find("value");
    if (attr != option->attrs.end()) return attr->second;
    std::string text = GetTextContent(option);
    const size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}
DomNode *OwnerSelect(DomNode *option) {
    for (DomNode *cur = option->parent; cur; cur = cur->parent) if (cur->tag == "select") return cur;
    return nullptr;
}
bool OptionExplicitlySelected(DomNode *option) {
    auto live = option->attrs.find("\x01selected");
    return live != option->attrs.end() ? live->second == "1" : option->attrs.count("selected") != 0;
}
bool OptionSelected(DomNode *option) {
    if (OptionExplicitlySelected(option)) return true;
    DomNode *select = OwnerSelect(option);
    if (!select || select->attrs.count("multiple")) return false;
    std::vector<DomNode *> options;
    CollectOptions(select, options);
    for (DomNode *other : options) if (OptionExplicitlySelected(other)) return false;
    return !options.empty() && options.front() == option;  // nothing chosen: the first one shows
}
void SetOptionSelected(DomNode *option, bool selected) {
    DomNode *select = OwnerSelect(option);
    if (select && selected && !select->attrs.count("multiple")) {
        std::vector<DomNode *> options;
        CollectOptions(select, options);
        for (DomNode *other : options) other->attrs["\x01selected"] = "0";
    }
    option->attrs["\x01selected"] = selected ? "1" : "0";
    if (select && selected) select->form_value = OptionValue(option);
}
std::string SelectValue(DomNode *select) {
    std::vector<DomNode *> options;
    CollectOptions(select, options);
    for (DomNode *option : options) if (OptionSelected(option)) return OptionValue(option);
    return "";
}
void SetSelectValue(DomNode *select, const std::string &value) {
    std::vector<DomNode *> options;
    CollectOptions(select, options);
    bool found = false;
    for (DomNode *option : options) {
        const bool match = !found && OptionValue(option) == value;
        option->attrs["\x01selected"] = match ? "1" : "0";
        found = found || match;
    }
    select->form_value = found ? value : "";
}

// "fooBar" <-> "data-foo-bar" for element.dataset.
std::string DatasetAttrName(const std::string &key) {
    std::string out = "data-";
    for (char c : key) {
        if (std::isupper(static_cast<unsigned char>(c))) { out += '-'; out += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        else out += c;
    }
    return out;
}

// Attributes that read and write straight through as same-named string
// properties (`a.href`, `input.type`, `img.src`, `el.id`, ...).
bool IsReflectedAttr(const std::string &key) {
    static const std::unordered_set<std::string> kReflected = {"id", "href", "src", "type", "name", "placeholder", "alt", "title", "rel", "target",
                                                                "action", "method", "lang", "dir", "role", "min", "max", "step", "pattern", "accept"};
    return kReflected.count(key) != 0;
}
bool IsReflectedBoolAttr(const std::string &key) {
    static const std::unordered_set<std::string> kReflected = {"disabled", "hidden", "readOnly", "required", "multiple", "selected", "autofocus"};
    return kReflected.count(key) != 0;
}
std::string LowerAscii(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The document being scripted and its interpreter, for DOM natives that
// have to call back into script (click() -> listeners) or retire nodes.
struct Interpreter;
thread_local Interpreter *g_active_interp = nullptr;
thread_local HtmlDoc *g_active_doc = nullptr;

// Nodes a script drops from the tree are parked, never freed, while the
// page lives: wrappers, listeners and script variables hold raw pointers.
void RetireChildren(DomNode *node) {
    if (g_active_doc) {
        for (auto &child : node->children) { child->parent = nullptr; g_active_doc->detached_nodes.push_back(std::move(child)); }
    }
    node->children.clear();
}

Value GetProp(const ObjectPtr &obj, const std::string &key) {
    if (!obj) return Value::Undef();
    if (obj->dataset_node) {
        auto attr = obj->dataset_node->attrs.find(DatasetAttrName(key));
        return attr == obj->dataset_node->attrs.end() ? Value::Undef() : Value::Str(attr->second);
    }
    // An ordinary (script-defined, non-class) function is a constructor:
    // its `prototype` object springs into being the first time anything
    // reads it (`F.prototype.m = ...`, `new F`, `x instanceof F`).
    if (key == "prototype" && obj->is_function && !obj->is_class && obj->fn_node && !obj->is_arrow_function && !obj->props.count("prototype")) {
        auto proto = std::make_shared<ObjectData>();
        proto->props["constructor"] = Value::Obj(obj);
        obj->props["prototype"] = Value::Obj(proto);
        return Value::Obj(proto);
    }
    if ((obj->is_map || obj->is_set) && key == "size") return Value::Num(static_cast<double>(obj->collection_entries.size()));
    if ((obj->is_map || obj->is_set) && (key == "has" || key == "get" || key == "set" || key == "add" || key == "delete" || key == "clear")) return MakeNativeFn([obj, key](const std::vector<Value> &args, bool &, std::string &) {
        auto found = [&]() { return args.empty() ? obj->collection_entries.end() : std::find_if(obj->collection_entries.begin(), obj->collection_entries.end(), [&](const auto &entry) { return StrictEquals(entry.first, args[0]); }); };
        if (key == "clear") { obj->collection_entries.clear(); return Value::Undef(); }
        auto it = found();
        if (key == "has") return Value::Bool(it != obj->collection_entries.end());
        if (key == "get") return it == obj->collection_entries.end() ? Value::Undef() : it->second;
        if (key == "delete") { if (it == obj->collection_entries.end()) return Value::Bool(false); obj->collection_entries.erase(it); return Value::Bool(true); }
        if (key == "set" || key == "add") {
            if (args.empty()) return Value::Obj(obj);
            Value value = key == "add" ? args[0] : (args.size() > 1 ? args[1] : Value::Undef());
            if (it == obj->collection_entries.end()) obj->collection_entries.emplace_back(args[0], value); else it->second = value;
            return Value::Obj(obj);
        }
        return Value::Undef();
    });
    if (obj->is_regexp && key == "test") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(obj->regexp && !args.empty() && obj->regexp->PartialMatch(ToDisplayString(args[0])));
    });
    if (obj->is_regexp && key == "exec") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        if (!obj->regexp || args.empty()) return Value::MakeNull();
        std::string text = ToDisplayString(args[0]);
        mep_regex::Match match = obj->regexp->Search(text, obj->regexp_global ? static_cast<int>(obj->regexp_last_index) : 0);
        if (!match.ok()) { if (obj->regexp_global) obj->regexp_last_index = 0; return Value::MakeNull(); }
        if (obj->regexp_global) { obj->regexp_last_index = match.end > match.start ? match.end : match.end + 1; obj->props["lastIndex"] = Value::Num(static_cast<double>(obj->regexp_last_index)); }
        auto result = std::make_shared<ObjectData>(); result->is_array = true;
        for (size_t i = 0; i < match.groups.size(); ++i) {
            const auto group = match.groups[i];
            result->props[std::to_string(i)] = group.first < 0 ? Value::Undef() : Value::Str(text.substr(static_cast<size_t>(group.first), static_cast<size_t>(group.second - group.first)));
        }
        result->props["length"] = Value::Num(static_cast<double>(match.groups.size()));
        result->props["index"] = Value::Num(static_cast<double>(match.start));
        result->props["input"] = Value::Str(text);
        Value groups = Value::Undef();
        for (size_t i = 1; i < obj->regexp_group_names.size() && i < match.groups.size(); ++i) {
            if (obj->regexp_group_names[i].empty()) continue;
            if (groups.type == VType::Undefined) groups = Value::Obj(std::make_shared<ObjectData>());
            groups.obj->props[obj->regexp_group_names[i]] = result->props[std::to_string(i)];
        }
        result->props["groups"] = groups;
        return Value::Obj(result);
    });
    if (obj->is_array && key == "push") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        long length = ArrayLength(obj); for (const Value &arg : args) obj->props[std::to_string(length++)] = arg;
        obj->props["length"] = Value::Num(static_cast<double>(length)); return Value::Num(static_cast<double>(length));
    });
    if (obj->is_array && key == "pop") return MakeNativeFn([obj](const std::vector<Value> &, bool &, std::string &) {
        long length = ArrayLength(obj); if (length == 0) return Value::Undef(); Value last = GetProp(obj, std::to_string(length - 1)); obj->props.erase(std::to_string(length - 1)); obj->props["length"] = Value::Num(static_cast<double>(length - 1)); return last;
    });
    if (obj->is_array && key == "shift") return MakeNativeFn([obj](const std::vector<Value> &, bool &, std::string &) {
        long length = ArrayLength(obj); if (length == 0) return Value::Undef(); Value first = GetProp(obj, "0");
        for (long i = 1; i < length; ++i) obj->props[std::to_string(i - 1)] = GetProp(obj, std::to_string(i));
        obj->props.erase(std::to_string(length - 1)); obj->props["length"] = Value::Num(static_cast<double>(length - 1)); return first;
    });
    if (obj->is_array && key == "unshift") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        long length = ArrayLength(obj); for (long i = length; i-- > 0;) obj->props[std::to_string(i + static_cast<long>(args.size()))] = GetProp(obj, std::to_string(i));
        for (size_t i = 0; i < args.size(); ++i) obj->props[std::to_string(i)] = args[i];
        length += static_cast<long>(args.size());
        obj->props["length"] = Value::Num(static_cast<double>(length));
        return Value::Num(static_cast<double>(length));
    });
    if (obj->is_array && key == "reverse") return MakeNativeFn([obj](const std::vector<Value> &, bool &, std::string &) {
        long length = ArrayLength(obj);
        for (long i = 0; i < length / 2; ++i) std::swap(obj->props[std::to_string(i)], obj->props[std::to_string(length - 1 - i)]);
        return Value::Obj(obj);
    });
    if (obj->is_array && key == "join") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        std::string separator = args.empty() ? "," : ToDisplayString(args[0]); std::string joined; long length = ArrayLength(obj);
        for (long i = 0; i < length; ++i) { if (i) joined += separator; Value value = GetProp(obj, std::to_string(i)); if (value.type != VType::Undefined && value.type != VType::Null) joined += ToDisplayString(value); }
        return Value::Str(joined);
    });
    if (obj->is_array && key == "indexOf") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::Num(-1);
        long length = ArrayLength(obj);
        for (long i = 0; i < length; ++i) if (StrictEquals(GetProp(obj, std::to_string(i)), args[0])) return Value::Num(static_cast<double>(i));
        return Value::Num(-1);
    });
    if (obj->is_array && key == "includes") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::Bool(false);
        long length = ArrayLength(obj); for (long i = 0; i < length; ++i) if (StrictEquals(GetProp(obj, std::to_string(i)), args[0])) return Value::Bool(true);
        return Value::Bool(false);
    });
    if (obj->is_array && key == "slice") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        long length = ArrayLength(obj), start = args.empty() ? 0 : static_cast<long>(ToNumber(args[0]));
        long end = args.size() < 2 ? length : static_cast<long>(ToNumber(args[1]));
        if (start < 0) start = std::max(0L, length + start); else start = std::min(start, length);
        if (end < 0) end = std::max(0L, length + end); else end = std::min(end, length);
        auto result = std::make_shared<ObjectData>(); result->is_array = true; long index = 0;
        for (long i = start; i < end; ++i) result->props[std::to_string(index++)] = GetProp(obj, std::to_string(i));
        result->props["length"] = Value::Num(static_cast<double>(index)); return Value::Obj(result);
    });
    if (obj->is_array && key == "concat") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>(); result->is_array = true; long index = 0;
        auto append = [&](const Value &value) { if (value.type == VType::Object && value.obj && value.obj->is_array) { for (long i = 0; i < ArrayLength(value.obj); ++i) result->props[std::to_string(index++)] = GetProp(value.obj, std::to_string(i)); } else result->props[std::to_string(index++)] = value; };
        append(Value::Obj(obj)); for (const Value &arg : args) append(arg); result->props["length"] = Value::Num(static_cast<double>(index)); return Value::Obj(result);
    });
    if (obj->is_array && key == "splice") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        const long length = ArrayLength(obj);
        long start = args.empty() ? 0 : static_cast<long>(ToNumber(args[0]));
        if (start < 0) start = std::max(0L, length + start); else start = std::min(start, length);
        long remove = args.size() < 2 ? length - start : std::max(0L, std::min(length - start, static_cast<long>(ToNumber(args[1]))));
        auto removed = std::make_shared<ObjectData>(); removed->is_array = true;
        for (long i = 0; i < remove; ++i) removed->props[std::to_string(i)] = GetProp(obj, std::to_string(start + i));
        removed->props["length"] = Value::Num(static_cast<double>(remove));
        std::vector<Value> tail;
        for (long i = start + remove; i < length; ++i) tail.push_back(GetProp(obj, std::to_string(i)));
        const long insertion = static_cast<long>(args.size() > 2 ? args.size() - 2 : 0);
        for (long i = start; i < length; ++i) obj->props.erase(std::to_string(i));
        for (long i = 0; i < insertion; ++i) obj->props[std::to_string(start + i)] = args[static_cast<size_t>(i + 2)];
        for (size_t i = 0; i < tail.size(); ++i) obj->props[std::to_string(start + insertion + static_cast<long>(i))] = tail[i];
        obj->props["length"] = Value::Num(static_cast<double>(length - remove + insertion));
        return Value::Obj(removed);
    });
    if (obj->is_array && key == "sort") return MakeNativeFn([obj](const std::vector<Value> &, bool &, std::string &) {
        std::vector<Value> values;
        for (long i = 0; i < ArrayLength(obj); ++i) values.push_back(GetProp(obj, std::to_string(i)));
        std::sort(values.begin(), values.end(), [](const Value &left, const Value &right) { return ToDisplayString(left) < ToDisplayString(right); });
        for (size_t i = 0; i < values.size(); ++i) obj->props[std::to_string(i)] = values[i];
        return Value::Obj(obj);
    });
    if (obj->is_array && key == "flat") return MakeNativeFn([obj](const std::vector<Value> &args, bool &, std::string &) {
        int depth = args.empty() ? 1 : std::max(0, static_cast<int>(ToNumber(args[0])));
        auto result = std::make_shared<ObjectData>(); result->is_array = true; long index = 0;
        std::function<void(const Value &, int)> append = [&](const Value &value, int remaining) {
            if (remaining > 0 && value.type == VType::Object && value.obj && value.obj->is_array) for (long i = 0; i < ArrayLength(value.obj); ++i) append(GetProp(value.obj, std::to_string(i)), remaining - 1);
            else result->props[std::to_string(index++)] = value;
        };
        for (long i = 0; i < ArrayLength(obj); ++i) append(GetProp(obj, std::to_string(i)), depth);
        result->props["length"] = Value::Num(static_cast<double>(index)); return Value::Obj(result);
    });
    if (obj->dom_node && key == "textContent") return Value::Str(GetTextContent(obj->dom_node));
    if (obj->dom_node && key == "innerHTML") { std::string html; for (const auto &child : obj->dom_node->children) html += SerializeDomNode(child.get()); return Value::Str(html); }
    if (obj->dom_node && key == "outerHTML") return Value::Str(SerializeDomNode(obj->dom_node));
    if (obj->dom_node && obj->owner_doc) {
        DomNode *node = obj->dom_node;
        if (key == "parentNode" || key == "parentElement") return node->parent ? WrapDomNode(*obj->owner_doc, node->parent) : Value::MakeNull();
        if (key == "firstChild" || key == "lastChild") {
            if (node->children.empty()) return Value::MakeNull();
            return WrapDomNode(*obj->owner_doc, (key == "firstChild" ? node->children.front() : node->children.back()).get());
        }
        if (key == "firstElementChild" || key == "lastElementChild") {
            if (key == "firstElementChild") for (const auto &child : node->children) if (child->type == DomNodeType::Element) return WrapDomNode(*obj->owner_doc, child.get());
            if (key == "lastElementChild") for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) if ((*it)->type == DomNodeType::Element) return WrapDomNode(*obj->owner_doc, it->get());
            return Value::MakeNull();
        }
        if (key == "nextSibling" || key == "previousSibling" || key == "nextElementSibling" || key == "previousElementSibling") {
            if (!node->parent) return Value::MakeNull();
            const auto &siblings = node->parent->children;
            for (size_t i = 0; i < siblings.size(); ++i) if (siblings[i].get() == node) {
                bool forward = key.find("next") == 0;
                bool elements_only = key.find("Element") != std::string::npos;
                for (size_t cursor = i; forward ? ++cursor < siblings.size() : cursor-- > 0;) {
                    if (!elements_only || siblings[cursor]->type == DomNodeType::Element) return WrapDomNode(*obj->owner_doc, siblings[cursor].get());
                }
                return Value::MakeNull();
            }
        }
        if (key == "children" || key == "childNodes") {
            auto array = std::make_shared<ObjectData>(); array->is_array = true; size_t count = 0;
            for (const auto &child : node->children) if (key == "childNodes" || child->type == DomNodeType::Element) array->props[std::to_string(count++)] = WrapDomNode(*obj->owner_doc, child.get());
            array->props["length"] = Value::Num(static_cast<double>(count)); return Value::Obj(array);
        }
        if (key == "className") return Value::Str(node->Class());
        if (node->tag == "select") {
            if (key == "value") return Value::Str(SelectValue(node));
            if (key == "options" || key == "selectedOptions" || key == "selectedIndex" || key == "length") {
                std::vector<DomNode *> options;
                CollectOptions(node, options);
                if (key == "length") return Value::Num(static_cast<double>(options.size()));
                if (key == "selectedIndex") { for (size_t i = 0; i < options.size(); ++i) if (OptionSelected(options[i])) return Value::Num(static_cast<double>(i)); return Value::Num(-1); }
                auto list = std::make_shared<ObjectData>();
                list->is_array = true;
                size_t count = 0;
                for (DomNode *option : options) if (key == "options" || OptionSelected(option)) list->props[std::to_string(count++)] = WrapDomNode(*obj->owner_doc, option);
                list->props["length"] = Value::Num(static_cast<double>(count));
                return Value::Obj(list);
            }
            if (key == "type") return Value::Str(node->attrs.count("multiple") ? "select-multiple" : "select-one");
        }
        if (node->tag == "option") {
            if (key == "value") return Value::Str(OptionValue(node));
            if (key == "selected") return Value::Bool(OptionSelected(node));
            if (key == "defaultSelected") return Value::Bool(node->attrs.count("selected") != 0);
            if (key == "text" || key == "label") return Value::Str(GetTextContent(node));
            if (key == "index") { std::vector<DomNode *> options; if (DomNode *select = OwnerSelect(node)) CollectOptions(select, options); for (size_t i = 0; i < options.size(); ++i) if (options[i] == node) return Value::Num(static_cast<double>(i)); return Value::Num(0); }
        }
        if (key == "defaultValue" && node->type == DomNodeType::Element) { auto attr = node->attrs.find("value"); return Value::Str(attr == node->attrs.end() ? "" : attr->second); }
        if (key == "defaultChecked") return Value::Bool(node->attrs.count("checked") != 0);
        if (node->tag == "img" && (key == "naturalWidth" || key == "naturalHeight" || key == "complete" || key == "width" || key == "height")) {
            int natural_w = 0, natural_h = 0;
            DomImageSize(*obj->owner_doc, node, natural_w, natural_h);
            if (key == "complete") return Value::Bool(true);  // loading is synchronous here
            const bool want_width = key == "naturalWidth" || key == "width";
            if (key == "width" || key == "height") { auto attr = node->attrs.find(key); if (attr != node->attrs.end() && std::atoi(attr->second.c_str()) > 0) return Value::Num(std::atoi(attr->second.c_str())); }
            return Value::Num(want_width ? natural_w : natural_h);
        }
        if (key == "isConnected") { DomNode *top = node; while (top->parent) top = top->parent; return Value::Bool(top == obj->owner_doc->root.get()); }
        if (key == "ownerDocument") { auto own = obj->props.find("ownerDocument"); if (own != obj->props.end()) return own->second; }
        if (key == "nodeValue" || key == "data") return node->type == DomNodeType::Element ? Value::MakeNull() : Value::Str(node->text);
        if (key == "innerText") return Value::Str(GetTextContent(node));
        if (key == "localName") return Value::Str(LowerAscii(node->tag));
        if (key == "childElementCount") { double count = 0; for (const auto &child : node->children) if (child->type == DomNodeType::Element) count++; return Value::Num(count); }
        if (node->type == DomNodeType::Element && IsReflectedAttr(key) && !obj->props.count(key)) {
            auto attr = node->attrs.find(key);
            return Value::Str(attr == node->attrs.end() ? "" : attr->second);
        }
        if (node->type == DomNodeType::Element && IsReflectedBoolAttr(key)) return Value::Bool(node->attrs.count(LowerAscii(key)) != 0);
        if (key == "htmlFor") { auto attr = node->attrs.find("for"); return Value::Str(attr == node->attrs.end() ? "" : attr->second); }
        if (key == "shadowRoot") return node->shadow_root ? WrapDomNode(*obj->owner_doc, node->shadow_root.get()) : Value::MakeNull();
        if (key == "tagName" || key == "nodeName") {
            if (node->type != DomNodeType::Element) return Value::Str(key == "nodeName" ? "#text" : "");
            // HTML elements report an uppercase name; SVG/other-namespace ones keep their case.
            for (DomNode *cur = node; cur; cur = cur->parent) if (cur->tag == "svg" || cur->tag == "math" || cur->attrs.count("xmlns")) return Value::Str(node->tag);
            std::string upper = node->tag;
            for (char &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return Value::Str(upper);
        }
        if (key == "nodeType") return Value::Num(node->type == DomNodeType::Element ? 1.0 : 3.0);
        if (key == "value") return Value::Str(node->form_value);
        if (key == "checked") return Value::Bool(node->form_checked);
        if (key == "open") return Value::Bool(node->details_open);
        if ((node->tag == "audio" || node->tag == "video") && key == "paused") return Value::Bool(node->media_paused);
        if ((node->tag == "audio" || node->tag == "video") && key == "muted") return Value::Bool(node->media_muted);
        if ((node->tag == "audio" || node->tag == "video") && key == "currentTime") return Value::Num(node->media_current_time);
        if ((node->tag == "audio" || node->tag == "video") && key == "volume") return Value::Num(node->media_volume);
        if ((node->tag == "audio" || node->tag == "video") && key == "duration") return Value::Num(node->media_ready_state >= 1 ? node->media_duration : std::numeric_limits<double>::quiet_NaN());
        if ((node->tag == "audio" || node->tag == "video") && key == "ended") return Value::Bool(node->media_ended);
        if ((node->tag == "audio" || node->tag == "video") && key == "readyState") return Value::Num(node->media_ready_state);
        if ((node->tag == "audio" || node->tag == "video") && key == "loop") return Value::Bool(node->attrs.count("loop") != 0);
        if ((node->tag == "audio" || node->tag == "video") && key == "error") {
            if (node->media_error.empty()) return Value::MakeNull();
            auto error = std::make_shared<ObjectData>();
            error->props["code"] = Value::Num(4);  // MEDIA_ERR_SRC_NOT_SUPPORTED
            error->props["message"] = Value::Str(node->media_error);
            return Value::Obj(error);
        }
        if (node->tag == "canvas" && key == "width") return Value::Num(node->canvas_width);
        if (node->tag == "canvas" && key == "height") return Value::Num(node->canvas_height);
    }
    if (obj->is_canvas_context) {
        if (key == "fillStyle" || key == "strokeStyle") {
            bool stroke = key == "strokeStyle";
            const std::shared_ptr<ObjectData> &gradient = stroke ? obj->canvas_stroke_gradient : obj->canvas_fill_gradient;
            if (gradient) return Value::Obj(gradient);
            unsigned char r = stroke ? obj->canvas_stroke_r : obj->canvas_r, g = stroke ? obj->canvas_stroke_g : obj->canvas_g, b = stroke ? obj->canvas_stroke_b : obj->canvas_b, a = stroke ? obj->canvas_stroke_a : obj->canvas_a;
            char text[40];
            // Serialization follows the spec: opaque colors as #rrggbb, others as rgba().
            if (a == 255) std::snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
            else std::snprintf(text, sizeof(text), "rgba(%d, %d, %d, %g)", r, g, b, static_cast<double>(a) / 255.0);
            return Value::Str(text);
        }
        if (key == "lineWidth") return Value::Num(static_cast<double>(obj->canvas_line_width));
        if (key == "globalAlpha") return Value::Num(static_cast<double>(obj->canvas_global_alpha));
        if (key == "font") return Value::Str(std::to_string(static_cast<int>(obj->canvas_font_size)) + "px monospace");
    }
    if (obj->style_node) {
        std::string css_key;
        for (char c : key) { if (std::isupper(static_cast<unsigned char>(c))) { css_key += '-'; css_key += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } else css_key += c; }
        const std::string &style = obj->style_node->attrs["style"];
        size_t pos = style.find(css_key + ":");
        if (pos != std::string::npos) {
            size_t start = pos + css_key.size() + 1; while (start < style.size() && std::isspace(static_cast<unsigned char>(style[start]))) ++start;
            size_t end = style.find(';', start); return Value::Str(style.substr(start, end == std::string::npos ? std::string::npos : end - start));
        }
    }
    if (obj->is_document && key == "title") return Value::Str(obj->owner_doc ? obj->owner_doc->title : "");
    if (obj->is_document && key == "cookie") {
        auto cookie = obj->props.find("cookie");
        return cookie == obj->props.end() ? Value::Str("") : cookie->second;
    }
    if (obj->is_document && obj->owner_doc && (key == "body" || key == "head" || key == "documentElement")) {
        const char *tag = key == "documentElement" ? "html" : key.c_str();
        DomNode *found = obj->owner_doc->root ? FindByTag(obj->owner_doc->root.get(), tag) : nullptr;
        return found ? WrapDomNode(*obj->owner_doc, found) : Value::MakeNull();
    }
    if (obj->is_document && obj->owner_doc && key == "activeElement") {
        std::function<DomNode *(DomNode *)> find_focused = [&](DomNode *current) -> DomNode * {
            if (!current) return nullptr;
            if (current->interaction_focus) return current;
            for (const auto &child : current->children) if (DomNode *found = find_focused(child.get())) return found;
            return current->shadow_root ? find_focused(current->shadow_root.get()) : nullptr;
        };
        DomNode *focused = find_focused(obj->owner_doc->root.get());
        return focused ? WrapDomNode(*obj->owner_doc, focused) : Value::MakeNull();
    }
    if (obj->is_class) {
        // A class's [[Prototype]] slot holds its instances' prototype (see
        // NodeKind::New); statics are found on the class and its bases.
        if (key == "prototype") return obj->prototype ? Value::Obj(obj->prototype) : Value::Undef();
        for (ObjectPtr klass = obj; klass; klass = klass->super_class) {
            auto it = klass->props.find(key);
            if (it != klass->props.end()) return it->second;
        }
    }
    for (ObjectPtr current = obj; current; current = current->prototype) {
        auto it = current->props.find(key);
        if (it != current->props.end()) return it->second;
    }
    return Value::Undef();
}

ObjectPtr FindAccessor(const ObjectPtr &obj, const std::string &key, bool setter) {
    for (ObjectPtr current = obj; current; current = current->prototype) {
        const auto &accessors = setter ? current->setters : current->getters;
        auto it = accessors.find(key);
        if (it != accessors.end()) return it->second;
    }
    return nullptr;
}

/**
 * @brief Writes a property on an object, resolving the magic textContent/title bindings and array-length bookkeeping before falling back to a plain property store.
 * @param obj The object to write to (a no-op if null).
 * @param key The property name to write.
 * @param val The value to store.
 */
void SetProp(const ObjectPtr &obj, const std::string &key, Value val) {
    if (!obj) return;
    if (obj->dom_node && key.size() > 2 && key[0] == 'o' && key[1] == 'n') SetDomEventHandler(obj, key, val);
    if (obj->frozen) return;
    if (obj->is_location && key == "href") {
        std::string href = ToDisplayString(val);
        // A relative assignment (history.pushState(s, '', '?view=about'),
        // location.href = 'next.html') resolves against the current URL,
        // and an absolute URL exposes its parts the way window.location does.
        const Value current = obj->props.count("href") ? obj->props["href"] : Value::Undef();
        if (current.type == VType::String && urlutil::HasScheme(current.str)) href = urlutil::ResolveUrl(current.str, href);
        const urlutil::ParsedUrl parsed = urlutil::ParseUrl(href);
        if (parsed.valid) {
            obj->props["href"] = Value::Str(href);
            obj->props["protocol"] = Value::Str(parsed.scheme + ":");
            obj->props["host"] = Value::Str(urlutil::HostWithPort(parsed));
            obj->props["hostname"] = Value::Str(parsed.host);
            obj->props["port"] = Value::Str(parsed.port ? std::to_string(parsed.port) : "");
            obj->props["origin"] = Value::Str(parsed.scheme == "file" ? "null" : urlutil::Origin(parsed));
            obj->props["pathname"] = Value::Str(parsed.path);
            obj->props["search"] = Value::Str(parsed.query);
            obj->props["hash"] = Value::Str(parsed.fragment);
            return;
        }
        obj->props["href"] = Value::Str(href);
        size_t hash = href.find('#'), query = href.find('?');
        const size_t path_end = std::min(query == std::string::npos ? href.size() : query, hash == std::string::npos ? href.size() : hash);
        obj->props["pathname"] = Value::Str(href.substr(0, path_end));
        obj->props["search"] = Value::Str(query == std::string::npos ? "" : href.substr(query, (hash == std::string::npos ? href.size() : hash) - query));
        obj->props["hash"] = Value::Str(hash == std::string::npos ? "" : href.substr(hash));
        return;
    }
    if (obj->is_window && key == "location" && obj->location_object) {
        SetProp(obj->location_object, "href", std::move(val));
        return;
    }
    if (obj->is_regexp && key == "lastIndex") {
        obj->regexp_last_index = std::max(0L, static_cast<long>(ToNumber(val)));
        obj->props["lastIndex"] = Value::Num(static_cast<double>(obj->regexp_last_index));
        return;
    }
    if (obj->dom_node && key == "textContent") {
        SetTextContent(obj->dom_node, ToDisplayString(val));
        return;
    }
    if (obj->dom_node && key == "innerHTML") { ReplaceInnerHtml(obj->dom_node, ToDisplayString(val)); return; }
    if (obj->dom_node && obj->dom_node->tag == "select" && key == "value") { SetSelectValue(obj->dom_node, ToDisplayString(val)); return; }
    if (obj->dom_node && obj->dom_node->tag == "select" && key == "selectedIndex") {
        std::vector<DomNode *> options;
        CollectOptions(obj->dom_node, options);
        const long wanted = static_cast<long>(ToNumber(val));
        for (size_t i = 0; i < options.size(); ++i) options[i]->attrs["\x01selected"] = static_cast<long>(i) == wanted ? "1" : "0";
        obj->dom_node->form_value = wanted >= 0 && static_cast<size_t>(wanted) < options.size() ? OptionValue(options[static_cast<size_t>(wanted)]) : "";
        return;
    }
    if (obj->dom_node && obj->dom_node->tag == "option" && key == "selected") { SetOptionSelected(obj->dom_node, val.Truthy()); return; }
    if (obj->dom_node && obj->dom_node->tag == "option" && key == "defaultSelected") { if (val.Truthy()) obj->dom_node->attrs["selected"] = ""; else obj->dom_node->attrs.erase("selected"); return; }
    if (obj->dom_node && obj->dom_node->tag == "option" && key == "value") { obj->dom_node->attrs["value"] = ToDisplayString(val); return; }
    if (obj->dom_node && key == "defaultValue") { obj->dom_node->attrs["value"] = ToDisplayString(val); return; }
    if (obj->dom_node && key == "defaultChecked") { if (val.Truthy()) obj->dom_node->attrs["checked"] = ""; else obj->dom_node->attrs.erase("checked"); return; }
    if (obj->dom_node && key == "value") { obj->dom_node->form_value = ToDisplayString(val); return; }
    if (obj->dom_node && key == "checked") { obj->dom_node->form_checked = val.Truthy(); return; }
    if (obj->dom_node && key == "open") { obj->dom_node->details_open = val.Truthy(); return; }
    if (obj->dom_node && (obj->dom_node->tag == "audio" || obj->dom_node->tag == "video")) {
        if (key == "muted") { obj->dom_node->media_muted = val.Truthy(); return; }
        if (key == "currentTime") { obj->dom_node->media_current_time = std::max(0.0, ToNumber(val)); return; }
        if (key == "volume") { obj->dom_node->media_volume = std::max(0.0, std::min(1.0, ToNumber(val))); return; }
        if (key == "loop") { if (val.Truthy()) obj->dom_node->attrs["loop"] = ""; else obj->dom_node->attrs.erase("loop"); return; }
    }
    if (obj->dom_node && obj->dom_node->tag == "canvas" && (key == "width" || key == "height")) {
        int value = std::max(1, std::min(static_cast<int>(ToNumber(val)), 8192));
        if (key == "width") obj->dom_node->canvas_width = value;
        else obj->dom_node->canvas_height = value;
        obj->dom_node->attrs[key] = std::to_string(value);
        obj->dom_node->canvas_commands.clear();  // HTML resets its bitmap when either dimension changes.
        return;
    }
    if (obj->dataset_node) { obj->dataset_node->attrs[DatasetAttrName(key)] = ToDisplayString(val); return; }
    if (obj->dom_node && obj->dom_node->type == DomNodeType::Element && IsReflectedAttr(key)) { obj->dom_node->attrs[key] = ToDisplayString(val); return; }
    if (obj->dom_node && obj->dom_node->type == DomNodeType::Element && IsReflectedBoolAttr(key)) {
        if (val.Truthy()) obj->dom_node->attrs[LowerAscii(key)] = ""; else obj->dom_node->attrs.erase(LowerAscii(key));
        return;
    }
    if (obj->dom_node && key == "htmlFor") { obj->dom_node->attrs["for"] = ToDisplayString(val); return; }
    if (obj->dom_node && (key == "nodeValue" || key == "data") && obj->dom_node->type != DomNodeType::Element) { obj->dom_node->text = ToDisplayString(val); return; }
    if (obj->dom_node && key == "className") { obj->dom_node->attrs["class"] = ToDisplayString(val); return; }
    if (obj->style_node) {
        std::string css_key;
        for (char c : key) { if (std::isupper(static_cast<unsigned char>(c))) { css_key += '-'; css_key += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } else css_key += c; }
        std::string &style = obj->style_node->attrs["style"];
        size_t pos = style.find(css_key + ":");
        std::string declaration = css_key + ": " + ToDisplayString(val) + ";";
        if (pos == std::string::npos) style += (style.empty() ? "" : " ") + declaration;
        else { size_t end = style.find(';', pos); style.replace(pos, end == std::string::npos ? std::string::npos : end - pos + 1, declaration); }
        obj->props[key] = std::move(val);
        return;
    }
    if (obj->is_canvas_context) {
        if (key == "lineWidth") { obj->canvas_line_width = std::max(0.0f, static_cast<float>(ToNumber(val))); return; }
        if (key == "font") { std::string font = ToDisplayString(val); char *end = nullptr; float size = std::strtof(font.c_str(), &end); if (end != font.c_str() && font.find("px") != std::string::npos) obj->canvas_font_size = std::max(1.0f, size); return; }
        if (key == "globalAlpha") { double alpha = ToNumber(val); if (alpha >= 0.0 && alpha <= 1.0) obj->canvas_global_alpha = static_cast<float>(alpha); return; }
        if (key == "fillStyle" || key == "strokeStyle") {
            bool stroke = key == "strokeStyle";
            std::shared_ptr<ObjectData> &gradient = stroke ? obj->canvas_stroke_gradient : obj->canvas_fill_gradient;
            if (val.type == VType::Object && val.obj && val.obj->is_canvas_gradient) { gradient = val.obj; return; }
            unsigned char r, g, b, a;
            // An unparseable color leaves the style untouched, as the spec requires.
            if (!ParseCssColor(ToDisplayString(val), r, g, b, a)) return;
            gradient.reset();
            if (stroke) { obj->canvas_stroke_r = r; obj->canvas_stroke_g = g; obj->canvas_stroke_b = b; obj->canvas_stroke_a = a; }
            else { obj->canvas_r = r; obj->canvas_g = g; obj->canvas_b = b; obj->canvas_a = a; }
            return;
        }
    }
    if (obj->is_document && key == "title") {
        if (obj->owner_doc) obj->owner_doc->title = ToDisplayString(val);
        return;
    }
    long idx;
    if (obj->is_array && IsArrayIndexKey(key, idx)) {
        obj->props[key] = val;
        long cur_len = ArrayLength(obj);
        if (idx + 1 > cur_len) obj->props["length"] = Value::Num(static_cast<double>(idx + 1));
        return;
    }
    obj->props[key] = std::move(val);
}

// ---------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------

enum class Tok {
    End,
    Num,
    Str,
    TemplateStr,
    Ident,
    KwVar,
    KwLet,
    KwConst,
    KwFunction,
    KwClass,
    KwExtends,
    KwNew,
    KwSuper,
    KwYield,
    PrivateIdent,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwOf,
    KwIn,
    KwSwitch,
    KwCase,
    KwDefault,
    KwTrue,
    KwFalse,
    KwNull,
    KwUndefined,
    KwBreak,
    KwContinue,
    KwTry,
    KwCatch,
    KwFinally,
    KwThrow,
    KwAsync,
    KwAwait,
    KwTypeof,
    KwInstanceof,
    KwDelete,
    KwVoid,
    KwDo,
    KwDebugger,
    Regex,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semicolon,
    Comma,
    Dot,
    Colon,
    Question,
    QuestionDot,
    Ellipsis,
    Arrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    Assign,
    EqEq,
    EqEqEq,
    NotEq,
    NotEqEq,
    Lt,
    Gt,
    LtEq,
    GtEq,
    AndAnd,
    OrOr,
    Nullish,
    Bang,
    PlusPlus,
    MinusMinus,
    PercentEq,
    StarStar,
    StarStarEq,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Shl,
    Shr,
    UShr,
    AmpEq,
    PipeEq,
    CaretEq,
    ShlEq,
    ShrEq,
    UShrEq,
    AndAndEq,
    OrOrEq,
    NullishEq,
};

struct Token {
    Tok type = Tok::End;
    std::string text;  // identifier name, string literal's decoded value, or raw text for TemplateStr (re-scanned by the parser -- see ParseTemplateLiteral)
    double num = 0;
    int pos = 0;  // byte offset in source, for error messages
    std::string flags;  // Tok::Regex only: the literal's flags ("gi")
};

struct Lexer {
    std::string src;
    size_t i = 0;
    // Whether a '/' at the next token position begins a regex literal
    // rather than a division: true at the start of input and after any
    // token that cannot end an operand (an operator, '(', ',', a keyword
    // like `return`), false after one that can (a literal, an identifier,
    // ')' or ']'). '}' counts as "can't end an operand" -- a block's
    // closing brace is followed by a statement far more often than an
    // object literal is divided. Maintained by Next().
    bool regex_allowed = true;
    // True when a line terminator was skipped before the token Next() just
    // returned -- what `return`/`break`/`continue`/postfix ++ need for
    // automatic semicolon insertion.
    bool newline_before = false;

    /**
     * @brief Constructs a lexer over the given source, positioned at offset 0.
     * @param s The source text to tokenize, moved into the lexer.
     */
    explicit Lexer(std::string s) : src(std::move(s)) {}

    /**
     * @brief Advances past whitespace, line comments, and block comments at the current position.
     */
    void SkipTrivia() {
        for (;;) {
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r' || src[i] == '\f' || src[i] == '\v')) {
                if (src[i] == '\n') newline_before = true;
                i++;
            }
            // U+00A0 (C2 A0), U+FEFF (EF BB BF) and U+2028/2029 (E2 80 A8/A9) are whitespace too.
            if (i + 1 < src.size() && static_cast<unsigned char>(src[i]) == 0xC2 && static_cast<unsigned char>(src[i + 1]) == 0xA0) { i += 2; continue; }
            if (i + 2 < src.size() && static_cast<unsigned char>(src[i]) == 0xEF && static_cast<unsigned char>(src[i + 1]) == 0xBB && static_cast<unsigned char>(src[i + 2]) == 0xBF) { i += 3; continue; }
            if (i + 2 < src.size() && static_cast<unsigned char>(src[i]) == 0xE2 && static_cast<unsigned char>(src[i + 1]) == 0x80 &&
                (static_cast<unsigned char>(src[i + 2]) == 0xA8 || static_cast<unsigned char>(src[i + 2]) == 0xA9)) { newline_before = true; i += 3; continue; }
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                    if (src[i] == '\n') newline_before = true;
                    i++;
                }
                i = std::min(src.size(), i + 2);
                continue;
            }
            break;
        }
    }

    /**
     * @brief Reads and decodes a single-quoted or double-quoted string literal body starting at the current position, consuming both delimiters.
     * @param quote The quote character that opens (and must close) the literal.
     * @return The decoded string contents, with \n \t \r \\ \' \" \` escapes resolved (any other escaped character is kept literally).
     */
    std::string ReadQuoted(char quote) {
        std::string out;
        i++;  // opening quote
        while (i < src.size() && src[i] != quote) {
            if (src[i] == '\\' && i + 1 < src.size()) {
                char c = src[i + 1];
                switch (c) {
                    case 'n':
                        out += '\n';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '\'':
                        out += '\'';
                        break;
                    case '"':
                        out += '"';
                        break;
                    case '`':
                        out += '`';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'v':
                        out += '\v';
                        break;
                    case '0':
                        out += '\0';
                        break;
                    case '\n':
                        break;  // line continuation
                    case '\r':
                        if (i + 2 < src.size() && src[i + 2] == '\n') i++;
                        break;
                    case 'x':
                    case 'u': {
                        // \xHH, \uHHHH, \u{H...}; a surrogate pair \uD83D\uDE00 combines into one code point.
                        size_t at = i + 2;
                        auto read_hex = [&](size_t count, unsigned &value) {
                            value = 0;
                            for (size_t k = 0; k < count; k++) {
                                if (at >= src.size() || !std::isxdigit(static_cast<unsigned char>(src[at]))) return false;
                                value = value * 16 + static_cast<unsigned>(std::stoi(std::string(1, src[at]), nullptr, 16));
                                at++;
                            }
                            return true;
                        };
                        unsigned cp = 0;
                        bool good = false;
                        if (c == 'x') good = read_hex(2, cp);
                        else if (at < src.size() && src[at] == '{') {
                            at++;
                            good = true;
                            while (at < src.size() && src[at] != '}') {
                                if (!std::isxdigit(static_cast<unsigned char>(src[at]))) { good = false; break; }
                                cp = cp * 16 + static_cast<unsigned>(std::stoi(std::string(1, src[at]), nullptr, 16));
                                at++;
                            }
                            if (good && at < src.size()) at++;
                        } else {
                            good = read_hex(4, cp);
                            if (good && cp >= 0xD800 && cp <= 0xDBFF && at + 1 < src.size() && src[at] == '\\' && src[at + 1] == 'u') {
                                const size_t save = at;
                                unsigned low = 0;
                                at += 2;
                                if (read_hex(4, low) && low >= 0xDC00 && low <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                else at = save;
                            }
                        }
                        if (!good) { out += c; break; }
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        i = at - 2;  // the shared `i += 2` below lands just past the escape
                        break;
                    }
                    default:
                        out += c;
                }
                i += 2;
            } else {
                out += src[i++];
            }
        }
        if (i < src.size()) i++;  // closing quote
        return out;
    }

    /**
     * @brief Scans and returns the next token, first skipping any leading trivia.
     * @return The next Token (Tok::End once the source is exhausted).
     */
    Token Next() {
        newline_before = false;
        Token t = NextRaw();
        SyncAfter(t);
        return t;
    }

    /**
     * @brief Sets regex_allowed as it stands right after token `t` -- also what the parser calls after rewinding `i` to just past an earlier token.
     * @param t The most recently consumed token.
     */
    void SyncAfter(const Token &t) {
        switch (t.type) {
            case Tok::Num: case Tok::Str: case Tok::TemplateStr: case Tok::Ident: case Tok::PrivateIdent: case Tok::Regex:
            case Tok::KwTrue: case Tok::KwFalse: case Tok::KwNull: case Tok::KwUndefined: case Tok::KwSuper:
            case Tok::RParen: case Tok::RBracket: case Tok::PlusPlus: case Tok::MinusMinus:
                regex_allowed = false;
                break;
            default:
                regex_allowed = true;
        }
    }

    /**
     * @brief Scans one token; Next() wraps this to keep regex_allowed/newline_before current.
     * @return The next Token (Tok::End once the source is exhausted).
     */
    Token NextRaw() {
        SkipTrivia();
        Token t;
        t.pos = static_cast<int>(i);
        if (i >= src.size()) {
            t.type = Tok::End;
            return t;
        }
        char c = src[i];
        if (c == '/' && regex_allowed) {
            // A regex literal: body up to the unescaped, un-bracketed '/', then flags.
            size_t at = i + 1;
            bool in_class = false;
            while (at < src.size() && src[at] != '\n') {
                if (src[at] == '\\' && at + 1 < src.size()) { at += 2; continue; }
                if (src[at] == '[') in_class = true;
                else if (src[at] == ']') in_class = false;
                else if (src[at] == '/' && !in_class) break;
                at++;
            }
            if (at < src.size() && src[at] == '/') {
                t.type = Tok::Regex;
                t.text = src.substr(i + 1, at - i - 1);
                at++;
                const size_t flags_start = at;
                while (at < src.size() && std::isalpha(static_cast<unsigned char>(src[at]))) at++;
                t.flags = src.substr(flags_start, at - flags_start);
                i = at;
                return t;
            }
        }
        if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X' || src[i + 1] == 'o' || src[i + 1] == 'O' || src[i + 1] == 'b' || src[i + 1] == 'B')) {
            const int base = (src[i + 1] == 'x' || src[i + 1] == 'X') ? 16 : ((src[i + 1] == 'o' || src[i + 1] == 'O') ? 8 : 2);
            i += 2;
            double value = 0;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
                if (src[i] != '_') {
                    const int digit = std::isdigit(static_cast<unsigned char>(src[i])) ? src[i] - '0' : (std::tolower(static_cast<unsigned char>(src[i])) - 'a' + 10);
                    if (digit >= base) break;
                    value = value * base + digit;
                }
                i++;
            }
            t.type = Tok::Num;
            t.num = value;
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t start = i;
            bool seen_dot = false;
            while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '_' || (src[i] == '.' && !seen_dot))) {
                if (src[i] == '.') seen_dot = true;
                i++;
            }
            if (i + 1 < src.size() && (src[i] == 'e' || src[i] == 'E') &&
                (std::isdigit(static_cast<unsigned char>(src[i + 1])) || ((src[i + 1] == '+' || src[i + 1] == '-') && i + 2 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 2]))))) {
                i++;
                if (i < src.size() && (src[i] == '+' || src[i] == '-')) i++;
                while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) i++;
            }
            std::string digits = src.substr(start, i - start);
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            if (i < src.size() && src[i] == 'n') i++;  // BigInt suffix: carried as a Number
            t.type = Tok::Num;
            t.num = std::strtod(digits.c_str(), nullptr);
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t start = i;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '$')) i++;
            std::string word = src.substr(start, i - start);
            static const std::unordered_map<std::string, Tok> kKeywords = {
                {"var", Tok::KwVar},       {"let", Tok::KwLet},         {"const", Tok::KwConst},
                {"function", Tok::KwFunction}, {"return", Tok::KwReturn}, {"if", Tok::KwIf},
                {"class", Tok::KwClass},     {"extends", Tok::KwExtends}, {"new", Tok::KwNew}, {"super", Tok::KwSuper}, {"yield", Tok::KwYield},
                {"else", Tok::KwElse},     {"while", Tok::KwWhile},     {"for", Tok::KwFor},
                {"in", Tok::KwIn},
                {"switch", Tok::KwSwitch}, {"case", Tok::KwCase},     {"default", Tok::KwDefault},
                {"true", Tok::KwTrue},     {"false", Tok::KwFalse},     {"null", Tok::KwNull},
                {"undefined", Tok::KwUndefined}, {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
                {"try", Tok::KwTry}, {"catch", Tok::KwCatch}, {"finally", Tok::KwFinally}, {"throw", Tok::KwThrow},
                {"async", Tok::KwAsync}, {"await", Tok::KwAwait},
                {"typeof", Tok::KwTypeof}, {"instanceof", Tok::KwInstanceof}, {"delete", Tok::KwDelete},
                {"void", Tok::KwVoid}, {"do", Tok::KwDo}, {"debugger", Tok::KwDebugger},
            };
            auto it = kKeywords.find(word);
            t.type = it != kKeywords.end() ? it->second : Tok::Ident;
            t.text = word;
            return t;
        }
        if (c == '#' && i + 1 < src.size() && (std::isalpha(static_cast<unsigned char>(src[i + 1])) || src[i + 1] == '_' || src[i + 1] == '$')) {
            const size_t start = i++;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '$')) ++i;
            t.type = Tok::PrivateIdent;
            t.text = src.substr(start, i - start);
            return t;
        }
        if (c == '"' || c == '\'') {
            t.type = Tok::Str;
            t.text = ReadQuoted(c);
            return t;
        }
        if (c == '`') {
            // Handed to the parser raw (with escapes still literal) -- it
            // re-scans this for ${...} interpolation boundaries itself
            // (ParseTemplateLiteral), since splitting that here would mean
            // the lexer producing a *sequence* of tokens for one backtick
            // literal, which doesn't fit this single-Token-per-Next() shape.
            size_t start = i + 1;
            i++;
            int depth = 0;
            while (i < src.size() && !(src[i] == '`' && depth == 0)) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    i += 2;
                    continue;
                }
                if (src[i] == '$' && i + 1 < src.size() && src[i + 1] == '{') depth++;
                if (src[i] == '}' && depth > 0) depth--;
                i++;
            }
            t.type = Tok::TemplateStr;
            t.text = src.substr(start, i - start);
            if (i < src.size()) i++;  // closing backtick
            return t;
        }
        /**
         * @brief Lexes a one- or two-character operator: consumes and emits two_tok if the next char is c2, otherwise consumes and emits just one_tok.
         * @param c2 The second character that, if present, extends the operator to two_tok.
         * @param two_tok The token type to emit when c2 follows.
         * @param one_tok The token type to emit otherwise.
         */
        auto two = [&](char c2, Tok two_tok, Tok one_tok) {
            if (i + 1 < src.size() && src[i + 1] == c2) {
                i += 2;
                t.type = two_tok;
            } else {
                i += 1;
                t.type = one_tok;
            }
        };
        switch (c) {
            case '(':
                i++;
                t.type = Tok::LParen;
                return t;
            case ')':
                i++;
                t.type = Tok::RParen;
                return t;
            case '{':
                i++;
                t.type = Tok::LBrace;
                return t;
            case '}':
                i++;
                t.type = Tok::RBrace;
                return t;
            case '[':
                i++;
                t.type = Tok::LBracket;
                return t;
            case ']':
                i++;
                t.type = Tok::RBracket;
                return t;
            case ';':
                i++;
                t.type = Tok::Semicolon;
                return t;
            case ',':
                i++;
                t.type = Tok::Comma;
                return t;
            case '.':
                if (i + 2 < src.size() && src[i + 1] == '.' && src[i + 2] == '.') {
                    i += 3;
                    t.type = Tok::Ellipsis;
                } else {
                    i++;
                    t.type = Tok::Dot;
                }
                return t;
            case ':':
                i++;
                t.type = Tok::Colon;
                return t;
            case '?':
                if (i + 1 < src.size() && src[i + 1] == '.' && !(i + 2 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 2])))) {
                    i += 2;
                    t.type = Tok::QuestionDot;
                } else if (i + 1 < src.size() && src[i + 1] == '?') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::NullishEq; }
                    else { i += 2; t.type = Tok::Nullish; }
                } else {
                    i++;
                    t.type = Tok::Question;
                }
                return t;
            case '+':
                if (i + 1 < src.size() && src[i + 1] == '+') {
                    i += 2;
                    t.type = Tok::PlusPlus;
                    return t;
                }
                two('=', Tok::PlusEq, Tok::Plus);
                return t;
            case '-':
                if (i + 1 < src.size() && src[i + 1] == '-') {
                    i += 2;
                    t.type = Tok::MinusMinus;
                    return t;
                }
                two('=', Tok::MinusEq, Tok::Minus);
                return t;
            case '*':
                if (i + 1 < src.size() && src[i + 1] == '*') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::StarStarEq; }
                    else { i += 2; t.type = Tok::StarStar; }
                    return t;
                }
                two('=', Tok::StarEq, Tok::Star);
                return t;
            case '%':
                two('=', Tok::PercentEq, Tok::Percent);
                return t;
            case '^':
                two('=', Tok::CaretEq, Tok::Caret);
                return t;
            case '~':
                i++;
                t.type = Tok::Tilde;
                return t;
            case '/':
                two('=', Tok::SlashEq, Tok::Slash);
                return t;
            case '<':
                if (i + 1 < src.size() && src[i + 1] == '<') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::ShlEq; }
                    else { i += 2; t.type = Tok::Shl; }
                    return t;
                }
                two('=', Tok::LtEq, Tok::Lt);
                return t;
            case '>':
                if (i + 2 < src.size() && src[i + 1] == '>' && src[i + 2] == '>') {
                    if (i + 3 < src.size() && src[i + 3] == '=') { i += 4; t.type = Tok::UShrEq; }
                    else { i += 3; t.type = Tok::UShr; }
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '>') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::ShrEq; }
                    else { i += 2; t.type = Tok::Shr; }
                    return t;
                }
                two('=', Tok::GtEq, Tok::Gt);
                return t;
            case '&':
                if (i + 1 < src.size() && src[i + 1] == '&') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::AndAndEq; }
                    else { i += 2; t.type = Tok::AndAnd; }
                    return t;
                }
                two('=', Tok::AmpEq, Tok::Amp);
                return t;
            case '|':
                if (i + 1 < src.size() && src[i + 1] == '|') {
                    if (i + 2 < src.size() && src[i + 2] == '=') { i += 3; t.type = Tok::OrOrEq; }
                    else { i += 2; t.type = Tok::OrOr; }
                    return t;
                }
                two('=', Tok::PipeEq, Tok::Pipe);
                return t;
            case '=':
                if (i + 2 < src.size() && src[i + 1] == '=' && src[i + 2] == '=') {
                    i += 3;
                    t.type = Tok::EqEqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '=') {
                    i += 2;
                    t.type = Tok::EqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '>') {
                    i += 2;
                    t.type = Tok::Arrow;
                    return t;
                }
                i++;
                t.type = Tok::Assign;
                return t;
            case '!':
                if (i + 2 < src.size() && src[i + 1] == '=' && src[i + 2] == '=') {
                    i += 3;
                    t.type = Tok::NotEqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '=') {
                    i += 2;
                    t.type = Tok::NotEq;
                    return t;
                }
                i++;
                t.type = Tok::Bang;
                return t;
            default:
                i++;
                t.type = Tok::End;  // unrecognized byte -- surfaces as an unexpected-end parse error rather than looping
                return t;
        }
    }
};

// ---------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------

enum class NodeKind {
    NumberLit, StringLit, BoolLit, NullLit, UndefinedLit, TemplateLit, ArrayLit, ObjectLit,
    Ident, Unary, Update, Binary, Logical, Assign, Member, Call, Conditional, FunctionExpr,
    ExprStmt, VarDecl, Block, If, While, For, Switch, Label, Return, Break, Continue, Throw, Try, Yield, Await, FunctionDecl, Class, New, TaggedCall, Program,
    Sequence,
    ImportDecl,     // a no-op at run time: bindings are made when the module is linked
    ExportDefault,  // export default <expr>;  a = the expression
    DynamicImport,  // import(<a>)
    ImportMeta,     // import.meta
 RegexLit, DoWhile,
};

struct Node;
// A declaration/assignment binding pattern.  Nodes are deliberately
// separate from expression AST nodes: `[a, {b: c = 1}]` names destinations,
// it is not an array/object value expression.
struct BindingPattern {
    enum class Kind { Ident, Array, Object } kind = Kind::Ident;
    std::string name;
    std::vector<std::unique_ptr<BindingPattern>> elements;  // null = array hole
    std::vector<std::pair<std::string, std::unique_ptr<BindingPattern>>> properties;
    std::unique_ptr<Node> default_value;
    // `...rest`: for an Array pattern the remaining elements, for an Object
    // pattern the properties not named by `properties`. Null = no rest.
    std::unique_ptr<BindingPattern> rest;
};

// Where the parser currently is, stamped onto each node it creates so a
// runtime error can quote the source it came from.
thread_local const std::string *g_parse_source = nullptr;
thread_local int g_parse_pos = 0;

struct Node {
    NodeKind kind;
    const std::string *source = g_parse_source;  // owned by the runtime (JsRuntime::sources)
    // Program (module): what it imports and exports, gathered while parsing.
    struct ModuleImport { std::string specifier, imported, local; };            // imported: "default", "*", a name; "" = side effect only
    struct ModuleExport { std::string exported, local, from_specifier, from_name; };  // exported "*" = export * from; from_name "*" = export * as ns from
    std::vector<ModuleImport> module_imports;
    std::vector<ModuleExport> module_exports;
    // VarDecl: source order across the two declarator lists (true = next pattern declarator).
    std::vector<bool> declarator_order;
    int pos = g_parse_pos;
    // Literals
    double num = 0;
    std::string str;
    bool boolean = false;  // BoolLit's value, or (reused) Update's prefix-vs-postfix flag -- true means prefix
    // TemplateLit: alternating literal-text parts (is_expr_part[k]==false) and expr parts (true)
    std::vector<bool> is_expr_part;
    std::vector<std::string> template_texts;
    std::vector<std::unique_ptr<Node>> template_exprs;
    // ArrayLit
    std::vector<std::unique_ptr<Node>> elements;
    std::vector<bool> element_spread;
    // ObjectLit
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> obj_props;
    // Aligned with obj_props: null is a static key, otherwise evaluates to
    // the computed key for `{[expr]: value}`.
    std::vector<std::unique_ptr<Node>> obj_prop_key_exprs;
    std::vector<bool> obj_prop_spread;
    // Aligned with obj_props: 0 = data property/method, 1 = getter, 2 = setter.
    std::vector<int> obj_prop_accessor;
    // Ident
    std::string name;
    // Unary/Binary/Logical/Assign
    std::string op;
    std::unique_ptr<Node> a, b, c;  // generic operand slots (unary: a; binary/logical/assign: a,b; conditional: a=cond,b=then,c=else)
    // Member
    bool computed = false;  // obj[expr] vs obj.prop
    bool optional = false;  // optional member/call (`?.`)
    std::string prop_name;
    // Call
    std::vector<std::unique_ptr<Node>> args;
    std::vector<bool> arg_spread;
    // FunctionExpr / FunctionDecl
    std::vector<std::string> params;
    std::vector<std::unique_ptr<Node>> param_defaults;  // aligned with params; null = no default
    // Also aligned with params; null denotes an ordinary identifier parameter.
    std::vector<std::unique_ptr<BindingPattern>> param_patterns;
    std::string rest_param;
    bool is_generator = false;
    bool is_async = false;
    std::vector<std::unique_ptr<Node>> body;  // Block's statement list, or a single implicit-return expr for a concise arrow body (see arrow_expr_body)
    bool arrow_expr_body = false;
    bool is_arrow = false;          // arrow function: no own `this`/`arguments`
    bool is_strict = false;         // defined in strict code: a plain call gets `this` undefined, not the window
    bool uses_arguments = false;    // the body mentions `arguments` (worth materializing it)
    // `var` names declared anywhere in this function/program body (not in
    // nested functions), computed once on first call -- what hoisting defines
    // up front. Filled lazily by HoistedVars().
    mutable std::vector<std::string> hoisted_vars;
    mutable bool hoisted_computed = false;
    // VarDecl
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> declarators;
    std::vector<std::pair<std::unique_ptr<BindingPattern>, std::unique_ptr<Node>>> pattern_declarators;
    // If/While/For/Block share a/b/c/body loosely; kept explicit per-kind below for clarity at eval time
    std::unique_ptr<Node> init, cond, update, then_branch, else_branch;
    // Switch cases: a null test denotes default; each body owns statements
    // until the next case/default label.
    std::vector<std::pair<std::unique_ptr<Node>, std::vector<std::unique_ptr<Node>>>> switch_cases;
    // Class: `a` is the optional base-class expression; body holds method
    // FunctionExpr nodes and names contains their corresponding names.
    std::vector<std::string> class_method_names;
    std::vector<bool> class_method_static;
    // 0 = ordinary method, 1 = getter, 2 = setter.
    std::vector<int> class_method_accessor;
    std::vector<std::string> class_private_fields;
    std::vector<std::unique_ptr<Node>> class_private_initializers;
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> class_static_fields;
    // Public instance fields (`x = 1;`), initialized per instance in order.
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> class_instance_fields;

    /**
     * @brief Constructs an AST node of the given kind, leaving all other fields at their defaults.
     * @param k The node's kind.
     */
    explicit Node(NodeKind k) : kind(k) {}
};

using NodePtr = std::unique_ptr<Node>;

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

struct Parser {
    Lexer lex;
    Token cur;
    bool ok = true;
    std::string error;

    /**
     * @brief Constructs a parser over the given source and primes it with the first token.
     * @param src The source text to parse, moved into the parser's lexer.
     */
    explicit Parser(std::string src) : lex(std::move(src)) { cur = lex.Next(); }

    /**
     * @brief Consumes the current token and lexes the next one into `cur`.
     */
    void Advance() { cur = lex.Next(); g_parse_pos = cur.pos; }

    /**
     * @brief Records a parse failure, keeping only the first one encountered.
     * @param msg The error message to record.
     */
    /** @brief True at the contextual keyword `of` (for...of); everywhere else `of` is a plain identifier. */
    bool IsOf() const { return cur.type == Tok::Ident && cur.text == "of"; }

    void Fail(const std::string &msg) {
        if (ok) {
            ok = false;
            // Where, and what the source looks like there: minified bundles
            // are one long line, so the excerpt matters more than the numbers.
            const std::string &src = lex.src;
            const size_t at = std::min(static_cast<size_t>(std::max(cur.pos, 0)), src.size());
            size_t line = 1, column = 1;
            for (size_t k = 0; k < at; ++k) { if (src[k] == '\n') { ++line; column = 1; } else ++column; }
            std::string excerpt = src.substr(at > 30 ? at - 30 : 0, (at > 30 ? 30 : at) + 40);
            for (char &c : excerpt) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            error = msg + " at " + std::to_string(line) + ":" + std::to_string(column) + " near `" + excerpt + "`";
        }
    }

    /**
     * @brief Tests whether the current token is of the given type, without consuming it.
     * @param t The token type to test against.
     * @return true if `cur`'s type equals t, false otherwise.
     */
    bool Check(Tok t) const { return cur.type == t; }

    /**
     * @brief Consumes the current token if it matches the given type.
     * @param t The token type to match.
     * @return true and advances past it if `cur`'s type equals t; false (leaving `cur` untouched) otherwise.
     */
    bool Match(Tok t) {
        if (cur.type == t) {
            Advance();
            return true;
        }
        return false;
    }

    /**
     * @brief Consumes the current token if it matches the given type, else records a parse failure.
     * @param t The required token type.
     * @param what A human-readable description of what was expected, used in the failure message.
     */
    void Expect(Tok t, const char *what) {
        if (!Match(t)) Fail(std::string("expected ") + what);
    }

    /**
     * @brief Parses a whole program: a sequence of statements until end of input.
     * @return The Program node containing the parsed top-level statements.
     */
    NodePtr ParseProgram() {
        auto prog = std::make_unique<Node>(NodeKind::Program);
        program = prog.get();
        if (Check(Tok::Str) && cur.text == "use strict") strict_mode = true;
        while (ok && !Check(Tok::End)) {
            prog->body.push_back(ParseStatement());
            if (!ok) break;
        }
        program = nullptr;
        return prog;
    }

    Node *program = nullptr;  // where import/export declarations are recorded
    bool strict_mode = false;  // inside "use strict" code or a module

    /** @brief At a `{`: whether the block opens with the "use strict" directive. */
    bool BodyStartsStrict() {
        if (!Check(Tok::LBrace)) return false;
        const size_t save_i = lex.i;
        const Token save_cur = cur;
        Advance();
        const bool directive = Check(Tok::Str) && cur.text == "use strict";
        lex.i = save_i;
        cur = save_cur;
        lex.SyncAfter(cur);
        g_parse_pos = cur.pos;
        return directive;
    }

    /** @brief Whether the token after the current one has the given type (nothing is consumed). */
    bool PeekIs(Tok type) {
        const size_t save_i = lex.i;
        const Token save_cur = cur;
        Advance();
        const bool matches = Check(type);
        lex.i = save_i;
        cur = save_cur;
        lex.SyncAfter(cur);
        g_parse_pos = cur.pos;
        return matches;
    }
    bool IsWordNamed(const char *word) const { return cur.type == Tok::Ident && cur.text == word; }

    /** @brief A module-level name in an import/export list: any word, or (for `export { x as "y" }`) a string. */
    std::string ParseModuleName() {
        if (!IsWord() && !Check(Tok::Str)) { Fail("expected a binding name"); return ""; }
        std::string name = cur.text;
        Advance();
        return name;
    }
    std::string ParseFromClause() {
        if (!IsWordNamed("from")) { Fail("expected 'from'"); return ""; }
        Advance();
        if (!Check(Tok::Str)) { Fail("expected a module specifier string"); return ""; }
        std::string specifier = cur.text;
        Advance();
        // import attributes (`with { type: "json" }`) are accepted and ignored
        if (IsWordNamed("with") || IsWordNamed("assert")) { Advance(); if (Check(Tok::LBrace)) { int depth = 0; do { if (Check(Tok::LBrace)) depth++; if (Check(Tok::RBrace)) depth--; Advance(); } while (ok && depth > 0 && !Check(Tok::End)); } }
        return specifier;
    }

    // import defaultName, { a, b as c } from 'm';  import * as ns from 'm';  import 'm';
    NodePtr ParseImportDecl() {
        Advance();  // import
        auto n = std::make_unique<Node>(NodeKind::ImportDecl);
        std::vector<Node::ModuleImport> found;
        if (Check(Tok::Str)) {
            found.push_back({cur.text, "", ""});
            Advance();
        } else {
            if (IsWord() && !Check(Tok::LBrace) && !Check(Tok::Star)) {
                found.push_back({"", "default", cur.text});
                Advance();
                Match(Tok::Comma);
            }
            if (Check(Tok::Star)) {
                Advance();
                if (!IsWordNamed("as")) Fail("expected 'as' after import *");
                else Advance();
                found.push_back({"", "*", ParseModuleName()});
            } else if (Check(Tok::LBrace)) {
                Advance();
                while (ok && !Check(Tok::RBrace) && !Check(Tok::End)) {
                    std::string imported = ParseModuleName(), local = imported;
                    if (IsWordNamed("as")) { Advance(); local = ParseModuleName(); }
                    found.push_back({"", imported, local});
                    if (!Match(Tok::Comma)) break;
                }
                Expect(Tok::RBrace, "'}'");
            }
            const std::string specifier = ParseFromClause();
            for (auto &entry : found) entry.specifier = specifier;
        }
        Match(Tok::Semicolon);
        if (program) for (auto &entry : found) program->module_imports.push_back(std::move(entry));
        return n;
    }

    // export const/let/var/function/class ...;  export default ...;  export { a, b as c } [from 'm'];  export * [as ns] from 'm';
    NodePtr ParseExportDecl() {
        Advance();  // export
        auto record = [this](std::string exported, std::string local, std::string from_specifier = "", std::string from_name = "") {
            if (program) program->module_exports.push_back({std::move(exported), std::move(local), std::move(from_specifier), std::move(from_name)});
        };
        if (Check(Tok::Star)) {
            Advance();
            std::string as_name;
            if (IsWordNamed("as")) { Advance(); as_name = ParseModuleName(); }
            const std::string specifier = ParseFromClause();
            Match(Tok::Semicolon);
            if (as_name.empty()) record("*", "", specifier, "");
            else record(as_name, "", specifier, "*");
            return std::make_unique<Node>(NodeKind::ImportDecl);
        }
        if (Check(Tok::LBrace)) {
            Advance();
            std::vector<std::pair<std::string, std::string>> names;  // local, exported
            while (ok && !Check(Tok::RBrace) && !Check(Tok::End)) {
                std::string local = ParseModuleName(), exported = local;
                if (IsWordNamed("as")) { Advance(); exported = ParseModuleName(); }
                names.emplace_back(local, exported);
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBrace, "'}'");
            std::string specifier;
            if (IsWordNamed("from")) specifier = ParseFromClause();
            Match(Tok::Semicolon);
            for (auto &name : names) {
                if (specifier.empty()) record(name.second, name.first);
                else record(name.second, "", specifier, name.first);
            }
            return std::make_unique<Node>(NodeKind::ImportDecl);
        }
        if (Check(Tok::KwDefault)) {
            Advance();
            // A named function/class is a declaration that also becomes the default export.
            const bool async_function = Check(Tok::KwAsync) && PeekIs(Tok::KwFunction);
            if ((Check(Tok::KwFunction) && (PeekIs(Tok::Ident) || PeekIs(Tok::Star))) || (Check(Tok::KwClass) && PeekIs(Tok::Ident)) || async_function) {
                NodePtr declaration = ParseStatement();
                if (!declaration->name.empty()) { record("default", declaration->name); return declaration; }
                Fail("export default declaration needs a name here");
                return declaration;
            }
            auto n = std::make_unique<Node>(NodeKind::ExportDefault);
            n->a = ParseAssignExpr();
            Match(Tok::Semicolon);
            record("default", "*default*");
            return n;
        }
        NodePtr declaration = ParseStatement();
        if (declaration->kind == NodeKind::VarDecl) {
            for (const auto &d : declaration->declarators) record(d.first, d.first);
            for (const auto &d : declaration->pattern_declarators) {
                std::vector<std::string> names;
                if (d.first) CollectBindingNames(*d.first, names);
                for (const std::string &name : names) record(name, name);
            }
        } else if (!declaration->name.empty()) {
            record(declaration->name, declaration->name);
        } else {
            Fail("unsupported export declaration");
        }
        return declaration;
    }
    static void CollectBindingNames(const BindingPattern &pattern, std::vector<std::string> &out) {
        if (pattern.kind == BindingPattern::Kind::Ident) { out.push_back(pattern.name); return; }
        for (const auto &element : pattern.elements) if (element) CollectBindingNames(*element, out);
        for (const auto &property : pattern.properties) if (property.second) CollectBindingNames(*property.second, out);
        if (pattern.rest) CollectBindingNames(*pattern.rest, out);
    }

    /**
     * @brief Parses a brace-delimited statement list: `{` stmt* `}`.
     * @return The Block node containing the parsed statements.
     */
    NodePtr ParseBlock() {
        Expect(Tok::LBrace, "'{'");
        auto blk = std::make_unique<Node>(NodeKind::Block);
        while (ok && !Check(Tok::RBrace) && !Check(Tok::End)) {
            blk->body.push_back(ParseStatement());
        }
        Expect(Tok::RBrace, "'}'");
        return blk;
    }

    /**
     * @brief Parses a single statement, dispatching on the current token to the right statement-kind parser (block, empty, var decl, function decl, if, while, for, return, break, continue, or an expression statement as the fallback).
     * @return The parsed statement node.
     */
    NodePtr ParseStatement() {
        if (!ok) return std::make_unique<Node>(NodeKind::Block);
        if (Check(Tok::LBrace)) return ParseBlock();
        if (Check(Tok::Semicolon)) {
            Advance();
            return std::make_unique<Node>(NodeKind::Block);  // empty statement, represented as an empty block
        }
        if (Check(Tok::KwVar) || Check(Tok::KwLet) || Check(Tok::KwConst)) return ParseVarDecl();
        if (Check(Tok::KwFunction)) return ParseFunctionDecl();
        // `import`/`export` declarations; `import(...)` and `import.meta` are expressions.
        if (IsWordNamed("import") && !PeekIs(Tok::LParen) && !PeekIs(Tok::Dot)) return ParseImportDecl();
        if (IsWordNamed("export") && (PeekIs(Tok::LBrace) || PeekIs(Tok::Star) || PeekIs(Tok::KwDefault) || PeekIs(Tok::KwVar) || PeekIs(Tok::KwLet) ||
                                      PeekIs(Tok::KwConst) || PeekIs(Tok::KwFunction) || PeekIs(Tok::KwClass) || PeekIs(Tok::KwAsync))) return ParseExportDecl();
        if (Check(Tok::KwAsync)) {
            const size_t save_i = lex.i;
            const Token save_cur = cur;
            Advance();
            if (Check(Tok::KwFunction)) {
                NodePtr function = ParseFunctionDecl();
                function->is_async = true;
                return function;
            }
            lex.i = save_i;
            cur = save_cur;
            lex.SyncAfter(cur);
        }
        if (Check(Tok::KwClass)) return ParseClassDecl();
        if (Check(Tok::KwIf)) return ParseIf();
        if (Check(Tok::KwWhile)) return ParseWhile();
        if (Check(Tok::KwDo)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::DoWhile);
            n->then_branch = ParseStatement();
            Expect(Tok::KwWhile, "'while' after do body");
            Expect(Tok::LParen, "'('");
            n->cond = ParseExpression();
            Expect(Tok::RParen, "')'");
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwDebugger)) {
            Advance();
            Match(Tok::Semicolon);
            return std::make_unique<Node>(NodeKind::Block);
        }
        if (Check(Tok::KwFor)) return ParseFor();
        if (Check(Tok::KwSwitch)) return ParseSwitch();
        if (Check(Tok::KwTry)) return ParseTry();
        if (Check(Tok::KwThrow)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Throw);
            n->a = ParseExpression();
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwReturn)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Return);
            // `return` followed by a line break returns undefined (automatic semicolon insertion).
            if (!Check(Tok::Semicolon) && !Check(Tok::RBrace) && !Check(Tok::End) && !lex.newline_before) n->a = ParseExpression();
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwBreak)) {
            Advance();
            std::string label;
            if (Check(Tok::Ident) && !lex.newline_before) { label = cur.text; Advance(); }
            Match(Tok::Semicolon);
            auto n = std::make_unique<Node>(NodeKind::Break);
            n->name = std::move(label);
            return n;
        }
        if (Check(Tok::KwContinue)) {
            Advance();
            std::string label;
            if (Check(Tok::Ident) && !lex.newline_before) { label = cur.text; Advance(); }
            Match(Tok::Semicolon);
            auto n = std::make_unique<Node>(NodeKind::Continue);
            n->name = std::move(label);
            return n;
        }
        if (Check(Tok::Ident)) {
            const std::string label = cur.text;
            const size_t save_i = lex.i;
            const Token next = lex.Next();
            lex.i = save_i;
            lex.SyncAfter(cur);
            if (next.type == Tok::Colon) {
                Advance();
                Expect(Tok::Colon, "':'");
                NodePtr target = ParseStatement();
                if (target->kind == NodeKind::For || target->kind == NodeKind::While || target->kind == NodeKind::DoWhile) {
                    target->name = label;
                    return target;
                }
                auto n = std::make_unique<Node>(NodeKind::Label);
                n->name = label;
                n->then_branch = std::move(target);
                return n;
            }
        }
        auto n = std::make_unique<Node>(NodeKind::ExprStmt);
        n->a = ParseExpression();
        Match(Tok::Semicolon);
        return n;
    }

    NodePtr ParseTry() {
        Expect(Tok::KwTry, "'try'");
        auto n = std::make_unique<Node>(NodeKind::Try);
        n->then_branch = ParseBlock();
        if (Match(Tok::KwCatch)) {
            if (Match(Tok::LParen)) {
                if (!Check(Tok::Ident)) Fail("expected catch binding");
                else { n->name = cur.text; Advance(); }
                Expect(Tok::RParen, "')' after catch binding");
            }
            n->else_branch = ParseBlock();
        }
        if (Match(Tok::KwFinally)) n->c = ParseBlock();
        if (!n->else_branch && !n->c) Fail("try requires catch or finally");
        return n;
    }

    std::unique_ptr<BindingPattern> ParseBindingPattern() {
        auto pattern = std::make_unique<BindingPattern>();
        if (Check(Tok::Ident)) {
            pattern->kind = BindingPattern::Kind::Ident;
            pattern->name = cur.text;
            Advance();
            return pattern;
        }
        if (Match(Tok::LBracket)) {
            pattern->kind = BindingPattern::Kind::Array;
            while (ok && !Check(Tok::RBracket)) {
                if (Match(Tok::Comma)) {
                    pattern->elements.push_back(nullptr);
                    continue;
                }
                if (Match(Tok::Ellipsis)) {
                    pattern->rest = ParseBindingPattern();
                    break;
                }
                auto element = ParseBindingPattern();
                if (Match(Tok::Assign)) element->default_value = ParseAssignExpr();
                pattern->elements.push_back(std::move(element));
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBracket, "']'");
            return pattern;
        }
        if (Match(Tok::LBrace)) {
            pattern->kind = BindingPattern::Kind::Object;
            while (ok && !Check(Tok::RBrace)) {
                if (Match(Tok::Ellipsis)) {
                    pattern->rest = ParseBindingPattern();
                    break;
                }
                if (!IsWord() && !Check(Tok::Str) && !Check(Tok::Num)) {
                    Fail("expected property key in binding pattern");
                    break;
                }
                std::string key = Check(Tok::Num) ? NumberToString(cur.num) : cur.text;
                Advance();
                std::unique_ptr<BindingPattern> value;
                if (Match(Tok::Colon)) value = ParseBindingPattern();
                else {
                    value = std::make_unique<BindingPattern>();
                    value->kind = BindingPattern::Kind::Ident;
                    value->name = key;
                }
                if (Match(Tok::Assign)) value->default_value = ParseAssignExpr();
                pattern->properties.emplace_back(std::move(key), std::move(value));
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBrace, "'}'");
            return pattern;
        }
        Fail("expected binding pattern");
        return pattern;
    }

    /**
     * @brief Parses a var/let/const declaration statement, including identifiers and array/object binding patterns.
     * @return The VarDecl node holding the parsed declarators.
     */
    NodePtr ParseVarDecl() {
        const bool is_var = Check(Tok::KwVar);
        Advance();  // var/let/const
        auto n = std::make_unique<Node>(NodeKind::VarDecl);
        n->boolean = is_var;  // true: function-scoped `var`; false: block-scoped let/const
        for (;;) {
            if (Check(Tok::LBracket) || Check(Tok::LBrace)) {
                auto pattern = ParseBindingPattern();
                NodePtr init;
                if (Match(Tok::Assign)) init = ParseAssignExpr();
                n->pattern_declarators.emplace_back(std::move(pattern), std::move(init));
                n->declarator_order.push_back(true);
            } else if (Check(Tok::Ident)) {
                std::string name = cur.text;
                Advance();
                NodePtr init;
                if (Match(Tok::Assign)) init = ParseAssignExpr();
                n->declarators.emplace_back(name, std::move(init));
                n->declarator_order.push_back(false);
            } else {
                Fail("expected identifier or binding pattern in declaration");
                break;
            }
            if (!Match(Tok::Comma)) break;
        }
        Match(Tok::Semicolon);
        return n;
    }

    /**
     * @brief Parses a named function declaration: `function name(params) { body }`.
     * @return The FunctionDecl node holding the function's name, parameters, and body.
     */
    NodePtr ParseFunctionDecl() {
        Advance();  // function
        auto n = std::make_unique<Node>(NodeKind::FunctionDecl);
        n->is_generator = Match(Tok::Star);
        if (Check(Tok::Ident)) {
            n->name = cur.text;
            Advance();
        } else {
            Fail("expected function name");
        }
        ParseParamsAndBody(*n);
        return n;
    }

    NodePtr ParseClassDecl() {
        Advance();  // class
        auto n = std::make_unique<Node>(NodeKind::Class);
        // The name is optional: `var A = class { ... }` / `class extends B {}`.
        if (Check(Tok::Ident)) {
            n->name = cur.text;
            Advance();
        }
        if (Match(Tok::KwExtends)) n->a = ParseCallOrMember();
        Expect(Tok::LBrace, "'{'");
        while (ok && !Check(Tok::RBrace)) {
            if (Match(Tok::Semicolon)) continue;
            if (Check(Tok::PrivateIdent)) {
                const std::string private_name = cur.text;
                Advance();
                if (Check(Tok::LParen)) {
                    // A private method: stored like a private field holding the function.
                    n->class_private_fields.push_back(private_name);
                    n->class_private_initializers.push_back(ParseMethod(private_name, false, false));
                    continue;
                }
                n->class_private_fields.push_back(private_name);
                if (Match(Tok::Assign)) n->class_private_initializers.push_back(ParseAssignExpr());
                else n->class_private_initializers.push_back(nullptr);
                Match(Tok::Semicolon);
                continue;
            }
            auto next_starts_member = [&]() {
                const size_t save_i = lex.i;
                const Token next = lex.Next();
                lex.i = save_i;
                lex.SyncAfter(cur);
                return next.type != Tok::LParen && next.type != Tok::Assign && next.type != Tok::Semicolon && next.type != Tok::RBrace;
            };
            bool is_static = false, is_async = false, is_generator = false;
            if (Check(Tok::Ident) && cur.text == "static" && next_starts_member()) { is_static = true; Advance(); }
            if (Check(Tok::KwAsync) && next_starts_member()) { is_async = true; Advance(); }
            if (Match(Tok::Star)) is_generator = true;
            int accessor = 0;
            if (Check(Tok::Ident) && (cur.text == "get" || cur.text == "set") && next_starts_member()) {
                accessor = cur.text == "get" ? 1 : 2;
                Advance();
            }
            std::string member_name;
            if (IsWord() || Check(Tok::Str)) member_name = cur.text;
            else if (Check(Tok::Num)) member_name = NumberToString(cur.num);
            else { Fail("expected class member name"); break; }
            Advance();
            if (!Check(Tok::LParen)) {
                // A field: `x = 1;`, `x;`, `static y = 2;`.
                NodePtr init;
                if (Match(Tok::Assign)) init = ParseAssignExpr();
                Match(Tok::Semicolon);
                if (is_static) n->class_static_fields.emplace_back(std::move(member_name), std::move(init));
                else n->class_instance_fields.emplace_back(std::move(member_name), std::move(init));
                continue;
            }
            n->body.push_back(ParseMethod(member_name, is_async, is_generator));
            n->class_method_names.push_back(std::move(member_name));
            n->class_method_static.push_back(is_static);
            n->class_method_accessor.push_back(accessor);
        }
        Expect(Tok::RBrace, "'}'");
        return n;
    }

    /**
     * @brief Parses a parenthesized parameter list followed by a brace-delimited body, filling them into an existing function node.
     * @param n The FunctionDecl/FunctionExpr node whose params and body are populated.
     */
    void ParseParamsAndBody(Node &n) {
        Expect(Tok::LParen, "'('");
        while (ok && !Check(Tok::RParen)) {
            if (Match(Tok::Ellipsis)) {
                if (!Check(Tok::Ident)) { Fail("expected rest parameter name"); break; }
                n.rest_param = cur.text;
                Advance();
                break;
            }
            if (Check(Tok::LBracket) || Check(Tok::LBrace)) {
                auto pattern = ParseBindingPattern();
                if (Match(Tok::Assign)) pattern->default_value = ParseAssignExpr();
                n.params.push_back("");
                n.param_defaults.push_back(nullptr);
                n.param_patterns.push_back(std::move(pattern));
            } else if (Check(Tok::Ident)) {
                n.params.push_back(cur.text);
                Advance();
                if (Match(Tok::Assign)) n.param_defaults.push_back(ParseAssignExpr());
                else n.param_defaults.push_back(nullptr);
                n.param_patterns.push_back(nullptr);
            } else {
                Fail("expected parameter name or binding pattern");
                break;
            }
            if (!Match(Tok::Comma)) break;
        }
        Expect(Tok::RParen, "')'");
        const bool outer_saw_arguments = saw_arguments;
        saw_arguments = false;
        const bool outer_strict = strict_mode;
        if (BodyStartsStrict()) strict_mode = true;
        n.is_strict = strict_mode;
        NodePtr blk = ParseBlock();
        strict_mode = outer_strict;
        n.body = std::move(blk->body);
        n.uses_arguments = saw_arguments;
        saw_arguments = outer_saw_arguments;
    }

    /**
     * @brief Parses an if statement, including its condition, then-branch, and optional else-branch.
     * @return The If node holding the parsed condition and branches.
     */
    NodePtr ParseIf() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::If);
        n->cond = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        if (Match(Tok::KwElse)) n->else_branch = ParseStatement();
        return n;
    }

    /**
     * @brief Parses a while statement, including its condition and loop body.
     * @return The While node holding the parsed condition and body.
     */
    NodePtr ParseWhile() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::While);
        n->cond = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        return n;
    }

    /**
     * @brief Parses a C-style for statement: `for (init; cond; update) body`, where init may be a var declaration or an expression, and cond/update are optional.
     * @return The For node holding the parsed init, cond, update, and body.
     */
    NodePtr ParseFor() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::For);
        // for (let value of iterable) / for (key in object).  Parse this
        // before the C-style initializer because `of`/`in` replace the
        // first semicolon entirely.
        if (Check(Tok::KwVar) || Check(Tok::KwLet) || Check(Tok::KwConst)) {
            const bool for_is_var = Check(Tok::KwVar);
            Advance();
            if (Check(Tok::LBracket) || Check(Tok::LBrace)) {
                // for (const [k, v] of pairs) / for (var {a} of list)
                auto pattern = ParseBindingPattern();
                if (IsOf() || Check(Tok::KwIn)) {
                    n->boolean = true;
                    n->op = IsOf() ? "of" : "in";
                    Advance();
                    n->pattern_declarators.emplace_back(std::move(pattern), nullptr);
                    n->a = ParseExpression();
                    Expect(Tok::RParen, "')'");
                    n->then_branch = ParseStatement();
                    return n;
                }
                auto decl = std::make_unique<Node>(NodeKind::VarDecl);
                decl->boolean = for_is_var;
                NodePtr pattern_init;
                no_in = true;
                if (Match(Tok::Assign)) pattern_init = ParseAssignExpr();
                no_in = false;
                decl->pattern_declarators.emplace_back(std::move(pattern), std::move(pattern_init));
                n->init = std::move(decl);
                Expect(Tok::Semicolon, "';'");
                goto for_tail;
            }
            if (!Check(Tok::Ident)) {
                Fail("expected identifier in for declaration");
                return n;
            }
            const std::string first_name = cur.text;
            Advance();
            if (IsOf() || Check(Tok::KwIn)) {
                n->name = first_name;
                n->boolean = true;  // declaration form
                n->op = IsOf() ? "of" : "in";
                Advance();
                n->a = ParseExpression();
                Expect(Tok::RParen, "')'");
                n->then_branch = ParseStatement();
                return n;
            }
            // Continue the ordinary C-style path after consuming the first
            // declaration name; ParseVarDecl cannot be reused here because
            // it expects to see the var/let/const keyword still.
            auto decl = std::make_unique<Node>(NodeKind::VarDecl);
            decl->boolean = for_is_var;
            NodePtr init;
            no_in = true;
            if (Match(Tok::Assign)) init = ParseAssignExpr();
            decl->declarators.emplace_back(first_name, std::move(init));
            while (Match(Tok::Comma)) {
                if (!Check(Tok::Ident)) { Fail("expected identifier in declaration"); break; }
                std::string name = cur.text;
                Advance();
                NodePtr value;
                if (Match(Tok::Assign)) value = ParseAssignExpr();
                decl->declarators.emplace_back(std::move(name), std::move(value));
            }
            no_in = false;
            n->init = std::move(decl);
            Expect(Tok::Semicolon, "';'");
        } else if (Check(Tok::Ident)) {
            const size_t save_i = lex.i;
            const Token save_cur = cur;
            const std::string name = cur.text;
            Advance();
            if (IsOf() || Check(Tok::KwIn)) {
                n->name = name;
                n->boolean = false;  // assign to an existing binding
                n->op = IsOf() ? "of" : "in";
                Advance();
                n->a = ParseExpression();
                Expect(Tok::RParen, "')'");
                n->then_branch = ParseStatement();
                return n;
            }
            lex.i = save_i;
            cur = save_cur;
            lex.SyncAfter(cur);
            auto es = std::make_unique<Node>(NodeKind::ExprStmt);
            no_in = true;
            es->a = ParseExpression();
            no_in = false;
            n->init = std::move(es);
            Expect(Tok::Semicolon, "';'");
        } else if (!Check(Tok::Semicolon)) {
            auto es = std::make_unique<Node>(NodeKind::ExprStmt);
            no_in = true;
            es->a = ParseExpression();
            no_in = false;
            n->init = std::move(es);
            Expect(Tok::Semicolon, "';'");
        } else {
            Advance();
        }
    for_tail:
        if (!Check(Tok::Semicolon)) n->cond = ParseExpression();
        Expect(Tok::Semicolon, "';'");
        if (!Check(Tok::RParen)) n->update = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        return n;
    }

    // Parses `switch (value) { case test: statements; default: statements }`.
    // Statements are stored per label rather than manufactured into nested
    // ifs, preserving JavaScript's fall-through behavior for evaluation.
    NodePtr ParseSwitch() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::Switch);
        n->cond = ParseExpression();
        Expect(Tok::RParen, "')'");
        Expect(Tok::LBrace, "'{'");
        while (ok && !Check(Tok::RBrace) && !Check(Tok::End)) {
            std::unique_ptr<Node> test;
            if (Match(Tok::KwCase)) {
                test = ParseExpression();
                Expect(Tok::Colon, "':'");
            } else if (Match(Tok::KwDefault)) {
                Expect(Tok::Colon, "':'");
            } else {
                Fail("expected case or default in switch");
                break;
            }
            std::vector<std::unique_ptr<Node>> body;
            while (ok && !Check(Tok::KwCase) && !Check(Tok::KwDefault) && !Check(Tok::RBrace)) {
                body.push_back(ParseStatement());
            }
            n->switch_cases.emplace_back(std::move(test), std::move(body));
        }
        Expect(Tok::RBrace, "'}'");
        return n;
    }

    // ---- Expressions, lowest to highest precedence ----

    /**
     * @brief Parses a full expression (the lowest-precedence entry point, currently equivalent to an assignment expression).
     * @return The parsed expression node.
     */
    NodePtr ParseExpression() {
        NodePtr first = ParseAssignExpr();
        if (!ok || !Check(Tok::Comma)) return first;
        // The comma operator: evaluate each, yield the last. Every list
        // context (arguments, array/object literals, declarators) parses
        // its items with ParseAssignExpr, so a comma only reaches here
        // where a whole Expression is allowed.
        auto seq = std::make_unique<Node>(NodeKind::Sequence);
        seq->body.push_back(std::move(first));
        while (ok && Match(Tok::Comma)) seq->body.push_back(ParseAssignExpr());
        return seq;
    }

    /**
     * @brief Whether `cur` can serve as a property name: an identifier or any keyword (`obj.default`, `{class: 1}`).
     * @return true for a word token.
     */
    bool IsWord() const {
        if (cur.type == Tok::Str || cur.type == Tok::TemplateStr || cur.type == Tok::Regex || cur.type == Tok::Num || cur.type == Tok::PrivateIdent) return false;
        return !cur.text.empty() && (std::isalpha(static_cast<unsigned char>(cur.text[0])) || cur.text[0] == '_' || cur.text[0] == '$');
    }

    /**
     * @brief Parses the params and body of a function-valued property/method whose name was already consumed.
     * @param name The method name.
     * @param is_async Whether `async` preceded it.
     * @param is_generator Whether `*` preceded it.
     * @return The FunctionExpr node.
     */
    NodePtr ParseMethod(const std::string &name, bool is_async, bool is_generator) {
        auto method = std::make_unique<Node>(NodeKind::FunctionExpr);
        method->name = name;
        method->is_async = is_async;
        method->is_generator = is_generator;
        ParseParamsAndBody(*method);
        return method;
    }

    // Set while parsing a for-loop's init clause, where a bare `in` belongs
    // to the for-in header rather than being the relational operator.
    bool no_in = false;
    // Whether the function body being parsed mentioned `arguments`
    // (saved/restored around each non-arrow function by ParseParamsAndBody).
    bool saw_arguments = false;

    /**
     * @brief Tests whether a token type is one of the supported assignment operators (=, +=, -=, *=, /=).
     * @param t The token type to test.
     * @return true if t is an assignment operator, false otherwise.
     */
    bool IsAssignOp(Tok t) const {
        switch (t) {
            case Tok::Assign: case Tok::PlusEq: case Tok::MinusEq: case Tok::StarEq: case Tok::SlashEq: case Tok::PercentEq:
            case Tok::StarStarEq: case Tok::AmpEq: case Tok::PipeEq: case Tok::CaretEq: case Tok::ShlEq: case Tok::ShrEq:
            case Tok::UShrEq: case Tok::AndAndEq: case Tok::OrOrEq: case Tok::NullishEq:
                return true;
            default:
                return false;
        }
    }

    /**
     * @brief Parses an assignment expression: first speculatively tries an arrow-function form (`ident =>` or `(params) =>`, rewinding the lexer if it doesn't pan out), otherwise parses a conditional expression and, if an assignment operator follows, wraps it as an Assign node.
     * @return The parsed expression node (a FunctionExpr for an arrow function, an Assign node for an assignment, or whatever ParseConditional produced otherwise).
     */
    NodePtr ParseAssignExpr() {
        if (Check(Tok::KwYield)) {
            // `yield`, `yield value`, `yield* iterable` -- an expression whose
            // own value is whatever the consumer passes to next().
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Yield);
            if (!lex.newline_before && Check(Tok::Star)) { Advance(); n->boolean = true; }
            const bool ends = Check(Tok::Semicolon) || Check(Tok::RBrace) || Check(Tok::RParen) || Check(Tok::RBracket) || Check(Tok::Comma) ||
                              Check(Tok::Colon) || Check(Tok::End) || lex.newline_before;
            if (!ends || n->boolean) n->a = ParseAssignExpr();
            return n;
        }
        // Arrow-function lookahead: `ident => ...` or `(params) => ...`.
        // A single bare identifier is easy to detect with one token of
        // lookahead; a parenthesized param list needs a full speculative
        // parse (cheap here -- scripts this engine targets are tiny), so
        // this snapshots the lexer position and rewinds if it turns out
        // not to be an arrow after all.
        if (Check(Tok::Ident)) {
            std::string maybe_name = cur.text;
            size_t save_i = lex.i;
            Token save_cur = cur;
            Advance();
            if (Check(Tok::Arrow)) {
                Advance();
                auto fn = std::make_unique<Node>(NodeKind::FunctionExpr);
                fn->params.push_back(maybe_name);
                ParseArrowBody(*fn);
                return fn;
            }
            lex.i = save_i;
            cur = save_cur;
            lex.SyncAfter(cur);
        } else if (Check(Tok::LParen)) {
            size_t save_i = lex.i;
            Token save_cur = cur;
            bool save_ok = ok;
            std::vector<std::string> params;
            std::vector<NodePtr> defaults;
            std::vector<std::unique_ptr<BindingPattern>> patterns;
            std::string rest_param;
            bool looks_like_params = true;
            Advance();
            while (ok && !Check(Tok::RParen)) {
                if (Match(Tok::Ellipsis)) {
                    if (!Check(Tok::Ident)) { looks_like_params = false; break; }
                    rest_param = cur.text;
                    Advance();
                    break;
                }
                if (Check(Tok::LBracket) || Check(Tok::LBrace)) {
                    auto pattern = ParseBindingPattern();
                    if (Match(Tok::Assign)) pattern->default_value = ParseAssignExpr();
                    params.push_back("");
                    defaults.push_back(nullptr);
                    patterns.push_back(std::move(pattern));
                } else if (!Check(Tok::Ident)) {
                    looks_like_params = false;
                    break;
                } else {
                    params.push_back(cur.text);
                    Advance();
                    if (Match(Tok::Assign)) defaults.push_back(ParseAssignExpr());
                    else defaults.push_back(nullptr);
                    patterns.push_back(nullptr);
                }
                if (!Match(Tok::Comma)) break;
            }
            if (looks_like_params && Check(Tok::RParen)) {
                Advance();
                if (Check(Tok::Arrow)) {
                    Advance();
                    auto fn = std::make_unique<Node>(NodeKind::FunctionExpr);
                    fn->params = std::move(params);
                    fn->param_defaults = std::move(defaults);
                    fn->param_patterns = std::move(patterns);
                    fn->rest_param = std::move(rest_param);
                    ParseArrowBody(*fn);
                    return fn;
                }
            }
            lex.i = save_i;
            cur = save_cur;
            lex.SyncAfter(cur);
            ok = save_ok;
            error.clear();
        }

        NodePtr left = ParseConditional();
        if (ok && IsAssignOp(cur.type)) {
            std::string op = cur.text;
            Tok t = cur.type;
            switch (t) {
                case Tok::Assign: op = "="; break;
                case Tok::PlusEq: op = "+="; break;
                case Tok::MinusEq: op = "-="; break;
                case Tok::StarEq: op = "*="; break;
                case Tok::SlashEq: op = "/="; break;
                case Tok::PercentEq: op = "%="; break;
                case Tok::StarStarEq: op = "**="; break;
                case Tok::AmpEq: op = "&="; break;
                case Tok::PipeEq: op = "|="; break;
                case Tok::CaretEq: op = "^="; break;
                case Tok::ShlEq: op = "<<="; break;
                case Tok::ShrEq: op = ">>="; break;
                case Tok::UShrEq: op = ">>>="; break;
                case Tok::AndAndEq: op = "&&="; break;
                case Tok::OrOrEq: op = "||="; break;
                default: op = "?\?="; break;
            }
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Assign);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseAssignExpr();
            return n;
        }
        return left;
    }

    /**
     * @brief Parses an arrow function's body, filling it into an existing function node: a brace-delimited block, or a single implicit-return expression (arrow_expr_body set true).
     * @param fn The FunctionExpr node whose body is populated.
     */
    void ParseArrowBody(Node &fn) {
        fn.is_arrow = true;
        if (Check(Tok::LBrace)) {
            NodePtr blk = ParseBlock();
            fn.body = std::move(blk->body);
        } else {
            fn.arrow_expr_body = true;
            fn.body.push_back(ParseAssignExpr());
        }
    }

    /**
     * @brief Parses a conditional (ternary) expression: a logical-OR expression optionally followed by `? then : else`.
     * @return A Conditional node if `?` was present, otherwise the parsed logical-OR expression unchanged.
     */
    NodePtr ParseConditional() {
        NodePtr cond = ParseNullish();
        if (Match(Tok::Question)) {
            auto n = std::make_unique<Node>(NodeKind::Conditional);
            n->a = std::move(cond);
            n->b = ParseAssignExpr();
            Expect(Tok::Colon, "':'");
            n->c = ParseAssignExpr();
            return n;
        }
        return cond;
    }

    /**
     * @brief Parses a left-associative chain of `||` logical-OR expressions.
     * @return The parsed expression, left-nested as Logical("||") nodes for each `||` encountered.
     */
    NodePtr ParseLogicalOr() {
        NodePtr left = ParseLogicalAnd();
        while (Check(Tok::OrOr)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "||";
            n->a = std::move(left);
            n->b = ParseLogicalAnd();
            left = std::move(n);
        }
        return left;
    }

    // Nullish coalescing short-circuits like || but only treats null and
    // undefined as absent; false, zero, and empty strings are retained.
    NodePtr ParseNullish() {
        NodePtr left = ParseLogicalOr();
        while (Check(Tok::Nullish)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "??";
            n->a = std::move(left);
            n->b = ParseLogicalOr();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of `&&` logical-AND expressions.
     * @return The parsed expression, left-nested as Logical("&&") nodes for each `&&` encountered.
     */
    NodePtr ParseLogicalAnd() {
        NodePtr left = ParseBitwise(0);
        while (Check(Tok::AndAnd)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "&&";
            n->a = std::move(left);
            n->b = ParseBitwise(0);
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses the three bitwise tiers, loosest first: `|` (tier 0), `^` (1), `&` (2), over equality operands.
     * @param tier Which tier to parse.
     * @return The resulting expression node.
     */
    NodePtr ParseBitwise(int tier) {
        if (tier > 2) return ParseEquality();
        const Tok tok = tier == 0 ? Tok::Pipe : tier == 1 ? Tok::Caret : Tok::Amp;
        const char *op = tier == 0 ? "|" : tier == 1 ? "^" : "&";
        NodePtr left = ParseBitwise(tier + 1);
        while (Check(tok)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseBitwise(tier + 1);
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of equality expressions; `==`/`===` and `!=`/`!==` are treated as equivalent (mapped to "==" / "!=") since this engine's comparisons are always strict.
     * @return The parsed expression, left-nested as Binary("==" or "!=") nodes for each operator encountered.
     */
    NodePtr ParseEquality() {
        NodePtr left = ParseRelational();
        for (;;) {
            std::string op;
            if (Check(Tok::EqEqEq)) op = "===";
            else if (Check(Tok::NotEqEq)) op = "!==";
            else if (Check(Tok::EqEq)) op = "==";
            else if (Check(Tok::NotEq)) op = "!=";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseRelational();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of relational expressions (`<`, `>`, `<=`, `>=`).
     * @return The parsed expression, left-nested as Binary nodes for each relational operator encountered.
     */
    NodePtr ParseRelational() {
        NodePtr left = ParseShift();
        for (;;) {
            std::string op;
            if (Check(Tok::KwInstanceof)) op = "instanceof";
            else if (Check(Tok::KwIn) && !no_in) op = "in";
            else if (Check(Tok::Lt)) op = "<";
            else if (Check(Tok::Gt))
                op = ">";
            else if (Check(Tok::LtEq))
                op = "<=";
            else if (Check(Tok::GtEq))
                op = ">=";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseShift();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of shift operators (<<, >>, >>>) over additive operands.
     * @return The resulting expression node.
     */
    NodePtr ParseShift() {
        NodePtr left = ParseAdditive();
        for (;;) {
            std::string op;
            if (Check(Tok::Shl)) op = "<<";
            else if (Check(Tok::Shr)) op = ">>";
            else if (Check(Tok::UShr)) op = ">>>";
            else break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseAdditive();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of additive expressions (`+`, `-`).
     * @return The parsed expression, left-nested as Binary nodes for each `+`/`-` encountered.
     */
    NodePtr ParseAdditive() {
        NodePtr left = ParseMultiplicative();
        for (;;) {
            std::string op;
            if (Check(Tok::Plus)) op = "+";
            else if (Check(Tok::Minus))
                op = "-";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseMultiplicative();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses a left-associative chain of multiplicative expressions (`*`, `/`, `%`).
     * @return The parsed expression, left-nested as Binary nodes for each `*`/`/`/`%` encountered.
     */
    NodePtr ParseMultiplicative() {
        NodePtr left = ParseExponent();
        for (;;) {
            std::string op;
            if (Check(Tok::Star)) op = "*";
            else if (Check(Tok::Slash))
                op = "/";
            else if (Check(Tok::Percent))
                op = "%";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseExponent();
            left = std::move(n);
        }
        return left;
    }

    /**
     * @brief Parses `**`, which is right-associative and binds tighter than the multiplicative operators.
     * @return The resulting expression node.
     */
    NodePtr ParseExponent() {
        NodePtr base = ParseUnary();
        if (!Check(Tok::StarStar)) return base;
        Advance();
        auto n = std::make_unique<Node>(NodeKind::Binary);
        n->op = "**";
        n->a = std::move(base);
        n->b = ParseExponent();
        return n;
    }

    /**
     * @brief Parses a unary expression: a prefix `-`/`+`/`!`/`typeof`, a prefix/postfix `++`/`--`, or (falling through) a call/member expression.
     * @return An Unary node for a `-`/`+`/`!`/`typeof` prefix, an Update node for `++`/`--` (prefix or postfix), or the parsed call/member expression otherwise.
     */
    NodePtr ParseUnary() {
        if (Check(Tok::KwAwait)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Await);
            n->a = ParseUnary();
            return n;
        }
        if (Check(Tok::Minus) || Check(Tok::Plus) || Check(Tok::Bang) || Check(Tok::KwTypeof) || Check(Tok::Tilde) || Check(Tok::KwVoid) || Check(Tok::KwDelete)) {
            std::string op = Check(Tok::Minus) ? "-" : Check(Tok::Plus) ? "+" : Check(Tok::Bang) ? "!" : Check(Tok::Tilde) ? "~"
                           : Check(Tok::KwVoid) ? "void" : Check(Tok::KwDelete) ? "delete" : "typeof";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Unary);
            n->op = op;
            n->a = ParseUnary();
            return n;
        }
        if (Check(Tok::PlusPlus) || Check(Tok::MinusMinus)) {
            std::string op = Check(Tok::PlusPlus) ? "++" : "--";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Update);
            n->op = op;
            n->boolean = true;  // prefix
            n->a = ParseUnary();
            return n;
        }
        NodePtr expr = ParseCallOrMember();
        if ((Check(Tok::PlusPlus) || Check(Tok::MinusMinus)) && !lex.newline_before) {
            std::string op = Check(Tok::PlusPlus) ? "++" : "--";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Update);
            n->op = op;
            n->boolean = false;  // postfix
            n->a = std::move(expr);
            return n;
        }
        return expr;
    }

    /**
     * @brief Parses a primary expression followed by any chain of member access (`.prop`, `[expr]`) and call (`(args)`) postfix operators.
     * @return The parsed expression, wrapped in Member/Call nodes for each postfix operator encountered, left to right.
     */
    NodePtr ParseCallOrMember() {
        NodePtr expr;
        if (Match(Tok::KwNew)) {
            auto construct = std::make_unique<Node>(NodeKind::New);
            NodePtr callee = Check(Tok::KwNew) ? ParseCallOrMember() : ParsePrimary();
            while (ok && (Check(Tok::Dot) || Check(Tok::LBracket))) {
                auto member = std::make_unique<Node>(NodeKind::Member);
                member->a = std::move(callee);
                if (Match(Tok::Dot)) {
                    if (!IsWord()) { Fail("expected property name after '.'"); break; }
                    member->prop_name = cur.text;
                    Advance();
                } else {
                    Advance();
                    member->b = ParseExpression();
                    member->computed = true;
                    Expect(Tok::RBracket, "']'");
                }
                callee = std::move(member);
            }
            construct->a = std::move(callee);
            if (Match(Tok::LParen)) {
                while (ok && !Check(Tok::RParen)) {
                    const bool spread = Match(Tok::Ellipsis);
                    construct->args.push_back(ParseAssignExpr());
                    construct->arg_spread.push_back(spread);
                    if (!Match(Tok::Comma)) break;
                }
                Expect(Tok::RParen, "')'");
            }
            expr = std::move(construct);
        } else {
            expr = ParsePrimary();
        }
        for (;;) {
            if (Match(Tok::Dot)) {
                if (!IsWord() && !Check(Tok::PrivateIdent)) {
                    Fail("expected property name after '.'");
                    return expr;
                }
                auto n = std::make_unique<Node>(NodeKind::Member);
                n->a = std::move(expr);
                n->prop_name = cur.text;
                n->computed = false;
                Advance();
                expr = std::move(n);
            } else if (Match(Tok::QuestionDot)) {
                if (Match(Tok::LBracket)) {
                    auto n = std::make_unique<Node>(NodeKind::Member);
                    n->a = std::move(expr);
                    n->b = ParseExpression();
                    n->computed = true;
                    n->optional = true;
                    Expect(Tok::RBracket, "']'");
                    expr = std::move(n);
                } else if (Match(Tok::LParen)) {
                    auto n = std::make_unique<Node>(NodeKind::Call);
                    n->a = std::move(expr);
                    n->optional = true;
                    while (ok && !Check(Tok::RParen)) {
                        const bool spread = Match(Tok::Ellipsis);
                        n->args.push_back(ParseAssignExpr());
                        n->arg_spread.push_back(spread);
                        if (!Match(Tok::Comma)) break;
                    }
                    Expect(Tok::RParen, "')'");
                    expr = std::move(n);
                } else if (IsWord()) {
                    auto n = std::make_unique<Node>(NodeKind::Member);
                    n->a = std::move(expr);
                    n->prop_name = cur.text;
                    n->optional = true;
                    Advance();
                    expr = std::move(n);
                } else {
                    Fail("expected property, '[' or '(' after '?.'");
                    return expr;
                }
            } else if (Match(Tok::LBracket)) {
                auto n = std::make_unique<Node>(NodeKind::Member);
                n->a = std::move(expr);
                n->b = ParseExpression();
                n->computed = true;
                Expect(Tok::RBracket, "']'");
                expr = std::move(n);
            } else if (Match(Tok::LParen)) {
                auto n = std::make_unique<Node>(NodeKind::Call);
                n->a = std::move(expr);
                while (ok && !Check(Tok::RParen)) {
                    const bool spread = Match(Tok::Ellipsis);
                    n->args.push_back(ParseAssignExpr());
                    n->arg_spread.push_back(spread);
                    if (!Match(Tok::Comma)) break;
                }
                Expect(Tok::RParen, "')'");
                expr = std::move(n);
            } else if (Check(Tok::TemplateStr)) {
                auto n = std::make_unique<Node>(NodeKind::TaggedCall);
                n->a = std::move(expr);
                n->b = ParseTemplateLiteral();
                expr = std::move(n);
            } else {
                break;
            }
        }
        return expr;
    }

    /**
     * @brief Parses a primary expression: a literal (number/string/template/bool/null/undefined), identifier, function expression, parenthesized expression, array literal, or object literal.
     * @return The parsed primary expression node; on an unrecognized token, records a parse failure and returns an UndefinedLit placeholder.
     */
    NodePtr ParsePrimary() {
        if (IsWordNamed("import") && (PeekIs(Tok::LParen) || PeekIs(Tok::Dot))) {
            Advance();
            if (Match(Tok::Dot)) {
                if (!IsWordNamed("meta")) Fail("expected import.meta");
                else Advance();
                return std::make_unique<Node>(NodeKind::ImportMeta);
            }
            Expect(Tok::LParen, "'('");
            auto n = std::make_unique<Node>(NodeKind::DynamicImport);
            n->a = ParseAssignExpr();
            if (Match(Tok::Comma) && !Check(Tok::RParen)) (void)ParseAssignExpr();  // import options: ignored
            Expect(Tok::RParen, "')'");
            return n;
        }
        if (Check(Tok::KwSuper)) {
            auto n = std::make_unique<Node>(NodeKind::Ident);
            n->name = "super";
            Advance();
            return n;
        }
        if (Check(Tok::Num)) {
            auto n = std::make_unique<Node>(NodeKind::NumberLit);
            n->num = cur.num;
            Advance();
            return n;
        }
        if (Check(Tok::Str)) {
            auto n = std::make_unique<Node>(NodeKind::StringLit);
            n->str = cur.text;
            Advance();
            return n;
        }
        if (Check(Tok::TemplateStr)) return ParseTemplateLiteral();
        if (Check(Tok::KwTrue) || Check(Tok::KwFalse)) {
            auto n = std::make_unique<Node>(NodeKind::BoolLit);
            n->boolean = Check(Tok::KwTrue);
            Advance();
            return n;
        }
        if (Check(Tok::KwNull)) {
            Advance();
            return std::make_unique<Node>(NodeKind::NullLit);
        }
        if (Check(Tok::KwUndefined)) {
            Advance();
            return std::make_unique<Node>(NodeKind::UndefinedLit);
        }
        if (Check(Tok::Ident) || IsOf()) {
            auto n = std::make_unique<Node>(NodeKind::Ident);
            n->name = cur.text;
            if (n->name == "arguments") saw_arguments = true;
            Advance();
            return n;
        }
        if (Check(Tok::Regex)) {
            auto n = std::make_unique<Node>(NodeKind::RegexLit);
            n->str = cur.text;
            n->op = cur.flags;
            Advance();
            return n;
        }
        if (Check(Tok::KwClass)) return ParseClassDecl();
        if (Check(Tok::KwAsync)) {
            Advance();
            if (Check(Tok::KwFunction)) {
                NodePtr fn = ParsePrimary();
                fn->is_async = true;
                return fn;
            }
            // `async x => ...` / `async (a, b) => ...`: the arrow itself is
            // parsed by ParseAssignExpr's own speculative path.
            NodePtr fn = ParseAssignExpr();
            if (fn && fn->kind == NodeKind::FunctionExpr) fn->is_async = true;
            else Fail("expected function after async");
            return fn;
        }
        if (Check(Tok::KwFunction)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::FunctionExpr);
            n->is_generator = Match(Tok::Star);
            if (Check(Tok::Ident)) {
                n->name = cur.text;
                Advance();
            }
            ParseParamsAndBody(*n);
            return n;
        }
        if (Match(Tok::LParen)) {
            NodePtr n = ParseExpression();
            Expect(Tok::RParen, "')'");
            return n;
        }
        if (Match(Tok::LBracket)) {
            auto n = std::make_unique<Node>(NodeKind::ArrayLit);
            while (ok && !Check(Tok::RBracket)) {
                const bool spread = Match(Tok::Ellipsis);
                n->elements.push_back(ParseAssignExpr());
                n->element_spread.push_back(spread);
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBracket, "']'");
            return n;
        }
        if (Match(Tok::LBrace)) {
            auto n = std::make_unique<Node>(NodeKind::ObjectLit);
            auto push = [&](std::string key, NodePtr value, NodePtr key_expr, bool spread, int accessor) {
                n->obj_props.emplace_back(std::move(key), std::move(value));
                n->obj_prop_key_exprs.push_back(std::move(key_expr));
                n->obj_prop_spread.push_back(spread);
                n->obj_prop_accessor.push_back(accessor);
            };
            while (ok && !Check(Tok::RBrace)) {
                if (Match(Tok::Ellipsis)) {
                    push("", ParseAssignExpr(), nullptr, true, 0);
                    if (!Match(Tok::Comma)) break;
                    continue;
                }
                // Modifiers: `async`, `*`, `get`/`set` -- each only when what
                // follows is another key rather than `:`, `(`, `,` or `}`
                // (so `{get: 1}`, `{async() {}}` and shorthand `{get}` still work).
                bool is_async = false, is_generator = false;
                int accessor = 0;
                auto next_starts_key = [&]() {
                    const size_t save_i = lex.i;
                    const Token next = lex.Next();
                    lex.i = save_i;
                    lex.SyncAfter(cur);
                    return next.type != Tok::Colon && next.type != Tok::LParen && next.type != Tok::Comma && next.type != Tok::RBrace && next.type != Tok::Assign;
                };
                if (Check(Tok::KwAsync) && next_starts_key()) { is_async = true; Advance(); }
                if (Match(Tok::Star)) is_generator = true;
                if (Check(Tok::Ident) && (cur.text == "get" || cur.text == "set") && next_starts_key()) {
                    accessor = cur.text == "get" ? 1 : 2;
                    Advance();
                }
                std::string key;
                NodePtr key_expr;
                bool shorthand_ok = false;
                if (Match(Tok::LBracket)) {
                    key_expr = ParseAssignExpr();
                    Expect(Tok::RBracket, "']'");
                } else if (IsWord()) {
                    shorthand_ok = Check(Tok::Ident) || IsOf() || Check(Tok::KwAsync) || Check(Tok::KwUndefined);
                    key = cur.text;
                    Advance();
                } else if (Check(Tok::Str)) {
                    key = cur.text;
                    Advance();
                } else if (Check(Tok::Num)) {
                    key = NumberToString(cur.num);
                    Advance();
                } else {
                    Fail("expected property key");
                    break;
                }
                if (accessor != 0 || is_async || is_generator || Check(Tok::LParen)) {
                    push(key, ParseMethod(key, is_async, is_generator), std::move(key_expr), false, accessor);
                } else if (Match(Tok::Colon)) {
                    push(key, ParseAssignExpr(), std::move(key_expr), false, 0);
                } else if (shorthand_ok && !key_expr) {
                    auto value = std::make_unique<Node>(NodeKind::Ident);
                    value->name = key;
                    // `{a = 1}` only means something as a destructuring target; keep the default.
                    if (Match(Tok::Assign)) {
                        auto with_default = std::make_unique<Node>(NodeKind::Assign);
                        with_default->op = "=";
                        with_default->a = std::move(value);
                        with_default->b = ParseAssignExpr();
                        push(key, std::move(with_default), nullptr, false, 0);
                    } else {
                        push(key, std::move(value), nullptr, false, 0);
                    }
                } else {
                    Fail("expected ':' after property key");
                    break;
                }
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBrace, "'}'");
            return n;
        }
        Fail("unexpected token in expression");
        return std::make_unique<Node>(NodeKind::UndefinedLit);
    }

    /**
     * @brief Parses the raw body a TemplateStr token captured, splitting it into alternating literal-text and `${...}` expression parts (each expression part re-parsed with a fresh sub-Parser) and decoding \n/\t escapes in the literal parts.
     * @return The TemplateLit node holding the alternating literal/expression parts.
     */
    NodePtr ParseTemplateLiteral() {
        // The lexer already isolated the raw `...${...}...` body (with
        // ${...} nesting balanced); re-scanning it here with its own
        // recursive Parser instances is simpler than threading template
        // interpolation through the main token stream.
        std::string raw = cur.text;
        Advance();
        auto n = std::make_unique<Node>(NodeKind::TemplateLit);
        std::string lit;
        size_t i = 0;
        while (i < raw.size()) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char c = raw[i + 1];
                if (c == 'n') lit += '\n';
                else if (c == 't')
                    lit += '\t';
                else
                    lit += c;
                i += 2;
                continue;
            }
            if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                n->is_expr_part.push_back(false);
                n->template_texts.push_back(lit);
                lit.clear();
                size_t start = i + 2;
                int depth = 1;
                size_t j = start;
                while (j < raw.size() && depth > 0) {
                    if (raw[j] == '{') depth++;
                    else if (raw[j] == '}')
                        depth--;
                    if (depth > 0) j++;
                }
                std::string expr_src = raw.substr(start, j - start);
                Parser sub(expr_src);
                NodePtr expr = sub.ParseExpression();
                if (!sub.ok) Fail("template literal: " + sub.error);
                n->is_expr_part.push_back(true);
                n->template_exprs.push_back(std::move(expr));
                n->template_texts.emplace_back();
                i = j + 1;
                continue;
            }
            lit += raw[i++];
        }
        n->is_expr_part.push_back(false);
        n->template_texts.push_back(lit);
        return n;
    }
};

// ---------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------

enum class CompletionType { Normal, Return, Break, Continue, Throw };

struct Completion {
    CompletionType type = CompletionType::Normal;
    Value value;
    std::string label;

    /**
     * @brief Constructs a Normal completion carrying an optional value.
     * @param v The completion's value (defaults to undefined).
     * @return A Completion with type CompletionType::Normal.
     */
    static Completion Norm(Value v = Value::Undef()) { return {CompletionType::Normal, std::move(v), ""}; }
    /**
     * @brief Constructs a Return completion carrying the returned value.
     * @param v The value being returned.
     * @return A Completion with type CompletionType::Return.
     */
    static Completion Ret(Value v) { return {CompletionType::Return, std::move(v), ""}; }
    /**
     * @brief Constructs a Break completion.
     * @return A Completion with type CompletionType::Break.
     */
    static Completion Brk(std::string label = "") { return {CompletionType::Break, Value::Undef(), std::move(label)}; }
    /**
     * @brief Constructs a Continue completion.
     * @return A Completion with type CompletionType::Continue.
     */
    static Completion Cont(std::string label = "") { return {CompletionType::Continue, Value::Undef(), std::move(label)}; }
    /**
     * @brief Constructs a Throw completion carrying an error message.
     * @param msg The thrown error message.
     * @return A Completion with type CompletionType::Throw, whose value is a string Value holding msg.
     */
    static Completion Thr(const std::string &msg) { return {CompletionType::Throw, Value::Str(msg), ""}; }
    static Completion Thr(Value value) { return {CompletionType::Throw, std::move(value), ""}; }

    /**
     * @brief Tests whether this completion is non-Normal (Return, Break, Continue, or Throw), i.e. should short-circuit further evaluation.
     * @return true if this completion's type is not CompletionType::Normal, false otherwise.
     */
    bool IsAbrupt() const { return type != CompletionType::Normal; }
};

// Bounds both "how many statements/loop-iterations/calls has this one
// script run" (an infinite `while(true){}` or unbounded recursion would
// otherwise hang mep -- it's single-threaded and synchronous, see
// js_engine.h) and native call-stack depth independently, since the two
// fail differently: a tight loop with no recursion never grows the C++
// stack at all, while deep recursion can blow it long before the step
// count above gets anywhere near its own limit. Both bounds are generous
// (a legitimate small script won't come close) but finite.
constexpr long kMaxSteps = 20'000'000;
constexpr int kMaxCallDepth = 300;

struct Interpreter {
    // A native reports failure as a message string; when the failure was a
    // script-thrown value passing through it (a generator's throw, a callback
    // that threw an Error object), the value itself rides along here.
    Value pending_throw;
    bool has_pending_throw = false;
    // import(): loads (or finds) the module at an absolute URL and returns its namespace object.
    std::function<Completion(const std::string &url)> load_module;
    std::string document_url;
    // Suspended async bodies, kept alive until they run to completion.
    std::unordered_map<struct Coroutine *, std::shared_ptr<struct Coroutine>> live_coroutines;
    // Set by a Member read that produced a script function: the object it was read from, for the enclosing Call's `this`.
    Value member_receiver;
    const ObjectData *member_receiver_fn = nullptr;
    // Engine-raised failures travel as plain strings; script code expects
    // `e.message`/`e instanceof Error`, so catch sites box them here.
    std::function<Value(const Value &)> wrap_thrown;
    Value WrapThrown(const Value &thrown) const { return (wrap_thrown && thrown.type == VType::String) ? wrap_thrown(thrown) : thrown; }
    long steps = 0;
    int call_depth = 0;
    EnvPtr global;
    std::vector<Value> *yield_values = nullptr;
    std::vector<ObjectPtr> microtasks;
    struct Timer {
        long id = 0;
        ObjectPtr callback;
        std::vector<Value> args;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds interval{0};
        bool repeat = false;
        bool animation_frame = false;
        bool canceled = false;
    };
    long next_timer_id = 1;
    std::vector<Timer> timers;
    // `window`: a bare identifier that isn't a declared binding resolves to
    // a property of this object (`window.React = ...` then `React`), and
    // top-level `this` is this object.
    ObjectPtr window_object;

    /**
     * @brief Increments the interpreter's step counter and checks it against the max-steps limit, guarding against infinite loops/unbounded execution.
     * @param out Set to a Throw completion (step-limit-exceeded error) when the limit is exceeded; left untouched otherwise.
     * @return true if the step limit was exceeded (caller should propagate `out`), false otherwise.
     */
    bool StepGuard(Completion &out) {
        if (++steps > kMaxSteps) {
            out = Completion::Thr("script exceeded step limit (possible infinite loop)");
            return true;
        }
        return false;
    }
};

struct DomEventListener {
    ObjectPtr callback;
    bool capture = false;
    bool once = false;
};

struct DomEventState {
    std::unordered_map<DomNode *, std::unordered_map<std::string, std::vector<DomEventListener>>> listeners;
    std::unordered_map<DomNode *, std::unordered_map<std::string, ObjectPtr>> property_handlers;
    // One wrapper per node for the life of the page: `a === b` for the same
    // element, and expando properties a script parks on a node survive.
    std::unordered_map<DomNode *, ObjectPtr> wrappers;
    // The key window-level listeners hang off; last stop of every bubble path.
    DomNode window_node;
    ObjectPtr document_object;  // what element.ownerDocument answers
    // Interface prototypes (HTMLInputElement.prototype, ...) by interface name; a wrapper's [[Prototype]].
    std::unordered_map<std::string, ObjectPtr> interface_protos;
};

std::shared_ptr<DomEventState> GetDomEventState(HtmlDoc &doc) {
    if (!doc.js_event_state) doc.js_event_state = std::make_shared<DomEventState>();
    return std::static_pointer_cast<DomEventState>(doc.js_event_state);
}

void SetDomEventHandler(const ObjectPtr &obj, const std::string &key, const Value &value) {
    if (!obj || !obj->dom_node || !obj->owner_doc) return;
    const std::string type = key.substr(2);
    auto state = GetDomEventState(*obj->owner_doc);
    if (value.type == VType::Object && value.obj && value.obj->is_function) state->property_handlers[obj->dom_node][type] = value.obj;
    else {
        auto node = state->property_handlers.find(obj->dom_node);
        if (node != state->property_handlers.end()) node->second.erase(type);
    }
}

Completion EvalExpr(Interpreter &interp, const Node &n, EnvPtr &env);
Completion ExecStmt(Interpreter &interp, const Node &n, EnvPtr &env);
Completion BindPattern(Interpreter &interp, const BindingPattern &pattern, Value value, EnvPtr &env);
void SettlePromise(Interpreter &interp, const ObjectPtr &promise, int state, Value value);
void HoistVars(const Node &owner, const std::vector<NodePtr> &body, Environment &scope);
std::string DescribeExpr(const Node &n);
Completion MemberGet(Interpreter &interp, const Value &base, const std::string &key);
bool IterateValues(Interpreter &interp, const Value &source, std::vector<Value> &out, std::string &error);
bool JsonStringify(Interpreter &interp, const Value &input, const std::string &indent, const std::string &current, std::string &out, bool &threw, std::string &error, int depth = 0);

// ---- coroutines ------------------------------------------------------------
// A generator or async function body runs on its own native stack so that
// `yield`/`await` can stop it mid-expression and a later next()/promise
// settlement can carry on from exactly there. The tree-walking evaluator
// keeps its state in C++ frames, so switching stacks (ucontext) is what
// makes that possible without rewriting it in continuation-passing style.
#if defined(MEP_JS_COROUTINES)
struct Coroutine {
    ucontext_t context{};
    ucontext_t resumer{};
    void *stack = nullptr;
    size_t stack_size = 0;
    std::function<Completion()> body;
    bool started = false;
    bool finished = false;
    bool running = false;
    bool is_async = false;
    // Generator protocol: what the consumer sent in and how (next/throw/return).
    Value sent;
    int sent_mode = 0;  // 0 next(value), 1 throw(value), 2 return(value)
    Value yielded;
    Completion result;
    int call_depth = 0;
    Coroutine *resumed_by = nullptr;
    std::function<void()> on_finish;  // async: settle the function's promise

    ~Coroutine() { if (stack) munmap(stack, stack_size); }
};
thread_local Coroutine *g_current_coroutine = nullptr;
thread_local Coroutine *g_starting_coroutine = nullptr;

void CoroutineEntry() {
    Coroutine *self = g_starting_coroutine;
    self->result = self->body();
    self->finished = true;
    self->body = nullptr;
    swapcontext(&self->context, &self->resumer);
}

// Runs the coroutine until it next suspends or finishes.
void ResumeCoroutine(Interpreter &interp, Coroutine &co) {
    if (co.finished || co.running) return;
    const int outer_depth = interp.call_depth;
    if (!co.started) {
        co.started = true;
        co.stack_size = 8u * 1024u * 1024u;  // address space only; pages commit on touch
        void *mapped = mmap(nullptr, co.stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (mapped == MAP_FAILED) { co.finished = true; co.result = Completion::Thr("out of memory starting a coroutine"); return; }
        co.stack = mapped;
        getcontext(&co.context);
        co.context.uc_stack.ss_sp = co.stack;
        co.context.uc_stack.ss_size = co.stack_size;
        co.context.uc_link = nullptr;
        makecontext(&co.context, CoroutineEntry, 0);
        g_starting_coroutine = &co;
        co.call_depth = 0;
    }
    co.resumed_by = g_current_coroutine;
    g_current_coroutine = &co;
    co.running = true;
    interp.call_depth = co.call_depth;
    swapcontext(&co.resumer, &co.context);
    co.running = false;
    co.call_depth = interp.call_depth;
    interp.call_depth = outer_depth;
    g_current_coroutine = co.resumed_by;
}

// Called on the coroutine's own stack: hand control back to whoever resumed it.
void SuspendCoroutine(Coroutine &co) { swapcontext(&co.context, &co.resumer); }

Value MakeIterResult(Value value, bool done) {
    auto result = std::make_shared<ObjectData>();
    result->props["value"] = std::move(value);
    result->props["done"] = Value::Bool(done);
    return Value::Obj(result);
}

// The object a generator function call returns: next/throw/return drive the coroutine.
Value MakeGeneratorObject(Interpreter &interp, std::function<Completion()> body) {
    auto co = std::make_shared<Coroutine>();
    co->body = std::move(body);
    auto generator = std::make_shared<ObjectData>();
    generator->is_list_iterator = true;  // its own [Symbol.iterator]() is itself
    auto drive = [&interp, co](int mode) {
        return MakeNativeFn([&interp, co, mode](const std::vector<Value> &args, bool &threw, std::string &error) {
            Value argument = args.empty() ? Value::Undef() : args[0];
            if (co->running) { threw = true; error = "generator is already running"; return Value::Undef(); }
            if (!co->started && mode != 0) {
                // Never started: throw()/return() finish it without running the body.
                co->started = true;
                co->finished = true;
                co->body = nullptr;
                if (mode == 1) { threw = true; error = ToDisplayString(argument); interp.pending_throw = argument; interp.has_pending_throw = true; return Value::Undef(); }
                return MakeIterResult(argument, true);
            }
            if (co->finished) {
                if (mode == 1) { threw = true; error = ToDisplayString(argument); interp.pending_throw = argument; interp.has_pending_throw = true; return Value::Undef(); }
                return MakeIterResult(mode == 2 ? argument : Value::Undef(), true);
            }
            co->sent = argument;
            co->sent_mode = mode;
            ResumeCoroutine(interp, *co);
            if (!co->finished) return MakeIterResult(co->yielded, false);
            if (co->result.type == CompletionType::Throw) {
                threw = true;
                error = ToDisplayString(co->result.value);
                interp.pending_throw = co->result.value;
                interp.has_pending_throw = true;
                return Value::Undef();
            }
            return MakeIterResult(co->result.value, true);
        });
    };
    generator->props["next"] = drive(0);
    generator->props["throw"] = drive(1);
    generator->props["return"] = drive(2);
    generator->props["@@iterator"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return NativeThis(); });
    for (const char *name : {"next", "throw", "return"}) generator->non_enumerable.insert(name);
    return Value::Obj(generator);
}
#endif  // MEP_JS_COROUTINES

// One-pass function hoisting: a direct-child `function foo(){}` in this
// block is bound before any statement runs, so sibling statements
// (including ones textually *before* it) can call it -- matches the
// common "helper defined lower in the same script" pattern real JS
// hoisting also allows, without implementing full hoisting semantics
// for var declarations too (those stay bound at their own statement,
// per this file's own header comment on var/let/const).
/**
 * @brief Executes a block's statement list against the given scope, first hoisting any direct-child function declarations so they're callable before their own textual position.
 * @param interp The interpreter, providing step-counting/call-depth state shared across the whole run.
 * @param stmts The statements to execute, in order.
 * @param env The scope to execute them in (function declarations are bound here; other statements may create nested scopes of their own).
 * @return Normal on falling off the end of the list, or the first abrupt (Return/Break/Continue/Throw) completion produced by a statement.
 */
Completion ExecBlockBody(Interpreter &interp, const std::vector<NodePtr> &stmts, EnvPtr &env) {
    for (const auto &s : stmts) {
        if (s->kind == NodeKind::FunctionDecl) {
            auto obj = std::make_shared<ObjectData>();
            obj->is_function = true;
            obj->is_generator_function = s->is_generator;
            obj->is_async_function = s->is_async;
            obj->fn_node = s.get();
            obj->closure = env;
            env->Define(s->name, Value::Obj(obj));
        }
    }
    for (const auto &s : stmts) {
        Completion c;
        if (interp.StepGuard(c)) return c;
        c = ExecStmt(interp, *s, env);
        if (c.IsAbrupt()) return c;
    }
    return Completion::Norm();
}

/**
 * @brief Invokes a callable object (native or user-defined) with the given arguments, enforcing the max call-depth guard for user-defined functions and translating a concise arrow body's expression result or a block body's Return completion into the call's result.
 * @param interp The interpreter, providing call-depth tracking shared across the whole run.
 * @param fn The callable object to invoke.
 * @param args The argument values to pass; missing trailing parameters bind to undefined.
 * @return Normal with the call's result value, or a Throw completion (not callable, call-depth exceeded, or an exception propagated from the callee).
 */
Completion CallFunction(Interpreter &interp, const ObjectPtr &fn, std::vector<Value> &args, const Value *this_value = nullptr) {
    if (fn->native) {
        bool threw = false;
        std::string err;
        const Value *saved_this = g_native_this;
        g_native_this = this_value;
        interp.has_pending_throw = false;
        Value v = fn->native(args, threw, err);
        g_native_this = saved_this;
        if (threw && interp.has_pending_throw) {
            interp.has_pending_throw = false;
            return Completion::Thr(interp.pending_throw);
        }
        return threw ? Completion::Thr(err) : Completion::Norm(v);
    }
    if (!fn->fn_node) return Completion::Thr("value is not callable");
#if defined(MEP_JS_COROUTINES)
    if (fn->is_async_function && !fn->is_generator_function) {
        // Runs synchronously up to its first `await`, which suspends the
        // coroutine; the awaited promise's settlement resumes it.
        auto body = std::make_shared<ObjectData>(*fn);
        body->is_async_function = false;
        auto promise = std::make_shared<ObjectData>(); promise->is_promise = true;
        auto co = std::make_shared<Coroutine>();
        co->is_async = true;
        const bool has_this = this_value != nullptr;
        Value receiver = has_this ? *this_value : Value::Undef();
        std::vector<Value> call_args = args;
        Coroutine *raw = co.get();
        co->body = [&interp, body, call_args, receiver, has_this]() mutable { return CallFunction(interp, body, call_args, has_this ? &receiver : nullptr); };
        interp.live_coroutines[raw] = co;
        ResumeCoroutine(interp, *co);
        auto finish = [&interp, promise, raw]() {
            auto held = interp.live_coroutines.find(raw);
            if (held == interp.live_coroutines.end()) return;
            std::shared_ptr<Coroutine> keep = held->second;
            interp.live_coroutines.erase(held);
            SettlePromise(interp, promise, keep->result.type == CompletionType::Throw ? 2 : 1, keep->result.value);
        };
        if (co->finished) finish();
        else co->on_finish = finish;
        return Completion::Norm(Value::Obj(promise));
    }
    if (fn->is_generator_function) {
        ObjectPtr body = std::make_shared<ObjectData>(*fn);
        body->is_generator_function = false;
        body->is_async_function = false;
        const bool has_this = this_value != nullptr;
        Value receiver = has_this ? *this_value : Value::Undef();
        std::vector<Value> call_args = args;
        return Completion::Norm(MakeGeneratorObject(interp, [&interp, body, call_args, receiver, has_this]() mutable {
            return CallFunction(interp, body, call_args, has_this ? &receiver : nullptr);
        }));
    }
#endif
    if (fn->is_async_function) {
        auto body = std::make_shared<ObjectData>(*fn);
        body->is_async_function = false;
        Completion executed = CallFunction(interp, body, args, this_value);
        auto promise = std::make_shared<ObjectData>(); promise->is_promise = true;
        SettlePromise(interp, promise, executed.type == CompletionType::Throw ? 2 : 1, executed.value);
        return Completion::Norm(Value::Obj(promise));
    }
    if (fn->is_generator_function) {
        struct GeneratorState { bool started = false; size_t index = 0; std::vector<Value> values; };
        auto state = std::make_shared<GeneratorState>();
        ObjectPtr body = std::make_shared<ObjectData>(*fn);
        body->is_generator_function = false;
        Value receiver = this_value ? *this_value : Value::Undef();
        auto iterator = std::make_shared<ObjectData>();
        iterator->props["next"] = MakeNativeFn([&interp, state, body, args, receiver](std::vector<Value> &, bool &threw, std::string &err) mutable {
            if (!state->started) {
                state->started = true;
                std::vector<Value> call_args = args;
                std::vector<Value> *previous = interp.yield_values;
                interp.yield_values = &state->values;
                Completion ran = CallFunction(interp, body, call_args, &receiver);
                interp.yield_values = previous;
                if (ran.IsAbrupt()) { threw = true; err = ran.label; return Value::Undef(); }
            }
            auto result = std::make_shared<ObjectData>();
            if (state->index < state->values.size()) {
                result->props["value"] = state->values[state->index++];
                result->props["done"] = Value::Bool(false);
            } else {
                result->props["value"] = Value::Undef();
                result->props["done"] = Value::Bool(true);
            }
            return Value::Obj(result);
        });
        return Completion::Norm(Value::Obj(iterator));
    }
    if (++interp.call_depth > kMaxCallDepth) {
        interp.call_depth--;
        return Completion::Thr("script exceeded maximum call depth (possible unbounded recursion)");
    }
    EnvPtr scope = std::make_shared<Environment>();
    scope->parent = fn->closure;
    scope->is_function_scope = true;
    // An arrow function has no `this`/`arguments` of its own: leaving them
    // undefined here lets the lookup reach the enclosing function's.
    if (!fn->is_arrow_function) {
        // A plain call sees the window object in sloppy code, undefined in strict code.
        scope->Define("this", this_value ? *this_value : (interp.window_object && !fn->fn_node->is_strict ? Value::Obj(interp.window_object) : Value::Undef()));
        if (fn->fn_node->uses_arguments) {
            auto arguments = std::make_shared<ObjectData>();
            arguments->is_array = true;
            for (size_t i = 0; i < args.size(); ++i) arguments->props[std::to_string(i)] = args[i];
            arguments->props["length"] = Value::Num(static_cast<double>(args.size()));
            scope->Define("arguments", Value::Obj(arguments));
        }
    }
    if (fn->super_class) {
        ObjectPtr base_class = fn->super_class;
        Value super_value = MakeNativeFn([&interp, scope, base_class](std::vector<Value> &super_args, bool &threw, std::string &err) {
            Value base_constructor = base_class->is_class ? GetProp(base_class, "constructor") : Value::Obj(base_class);
            Value *receiver = scope->Find("this");
            if (base_constructor.type != VType::Object || !base_constructor.obj || !base_constructor.obj->is_function || !receiver) {
                threw = true;
                err = "super constructor is unavailable";
                return Value::Undef();
            }
            Completion called = CallFunction(interp, base_constructor.obj, super_args, receiver);
            if (called.IsAbrupt()) { threw = true; err = ToDisplayString(called.value); return Value::Undef(); }
            // A native base (Error, ...) builds its own object: adopt its fields.
            if (base_constructor.obj->native && called.value.type == VType::Object && called.value.obj && receiver->type == VType::Object && receiver->obj) {
                for (const auto &entry : called.value.obj->props) receiver->obj->props[entry.first] = entry.second;
            }
            return called.value;
        });
        // `super.method()` is a method lookup on a receiver-bound facade,
        // while bare `super(...)` remains the base constructor call above.
        ObjectPtr base_instance_proto = base_class->prototype;
        if (!base_class->is_class) { Value base_proto = GetProp(base_class, "prototype"); base_instance_proto = base_proto.type == VType::Object ? base_proto.obj : nullptr; }
        for (ObjectPtr proto = base_instance_proto; proto; proto = proto->prototype) {
            for (const auto &entry : proto->props) {
                if (super_value.obj->props.contains(entry.first)) continue;
                if (entry.second.type != VType::Object || !entry.second.obj || !entry.second.obj->is_function) continue;
                ObjectPtr base_method = entry.second.obj;
                super_value.obj->props[entry.first] = MakeNativeFn([&interp, scope, base_method](std::vector<Value> &method_args, bool &threw, std::string &err) {
                    Value *receiver = scope->Find("this");
                    if (!receiver) { threw = true; err = "super method receiver is unavailable"; return Value::Undef(); }
                    Completion called = CallFunction(interp, base_method, method_args, receiver);
                    if (called.IsAbrupt()) { threw = true; err = called.label; return Value::Undef(); }
                    return called.value;
                });
            }
        }
        scope->Define("super", std::move(super_value));
    }
    const Node &def = *fn->fn_node;
    for (size_t i = 0; i < def.params.size(); i++) {
        Value value = i < args.size() ? args[i] : Value::Undef();
        if (i < def.param_patterns.size() && def.param_patterns[i]) {
            Completion bound = BindPattern(interp, *def.param_patterns[i], value, scope);
            if (bound.IsAbrupt()) { interp.call_depth--; return bound; }
            continue;
        }
        if (value.type == VType::Undefined && i < def.param_defaults.size() && def.param_defaults[i]) {
            Completion fallback = EvalExpr(interp, *def.param_defaults[i], scope);
            if (fallback.IsAbrupt()) { interp.call_depth--; return fallback; }
            value = fallback.value;
        }
        scope->Define(def.params[i], std::move(value));
    }
    if (!def.rest_param.empty()) {
        auto rest = std::make_shared<ObjectData>();
        rest->is_array = true;
        size_t rest_index = 0;
        for (size_t i = def.params.size(); i < args.size(); ++i) rest->props[std::to_string(rest_index++)] = args[i];
        rest->props["length"] = Value::Num(static_cast<double>(rest_index));
        scope->Define(def.rest_param, Value::Obj(rest));
    }
    if (def.arrow_expr_body) {
        // A concise arrow body's expression value *is* the return value --
        // unlike a block body, there's no explicit `return` to produce a
        // Return completion, so this returns straight from the expression's
        // own (already-Normal-or-Throw) completion rather than falling
        // through to the Return-completion check below, which would never
        // match and silently discard the value as undefined.
        Completion body_result = EvalExpr(interp, *def.body[0], scope);
        interp.call_depth--;
        if (body_result.type == CompletionType::Throw) return body_result;
        return Completion::Norm(body_result.value);
    }
    HoistVars(def, def.body, *scope);
    Completion result = ExecBlockBody(interp, def.body, scope);
    interp.call_depth--;
    if (result.type == CompletionType::Return) return Completion::Norm(result.value);
    if (result.type == CompletionType::Throw) return result;
    return Completion::Norm(Value::Undef());
}

// Microtasks deliberately share the interpreter and global scope of the
// script that queued them.  Drain the complete FIFO queue: a job may queue
// another job, which must run before control returns to the document loop.
Completion DrainMicrotasks(Interpreter &interp) {
    size_t next = 0;
    while (next < interp.microtasks.size()) {
        ObjectPtr job = interp.microtasks[next++];
        std::vector<Value> args;
        Completion result = CallFunction(interp, job, args);
        if (result.IsAbrupt()) {
            interp.microtasks.clear();
            return result;
        }
    }
    interp.microtasks.clear();
    return Completion::Norm();
}

Completion RunDueTimers(Interpreter &interp, bool *ran_any = nullptr) {
    const auto now = std::chrono::steady_clock::now();
    // Snapshot what is due, oldest deadline first; a callback may add or
    // clear timers, so each one is looked up again by id when its turn comes.
    std::vector<std::pair<std::chrono::steady_clock::time_point, long>> due;
    for (const Interpreter::Timer &timer : interp.timers) if (!timer.canceled && timer.deadline <= now) due.emplace_back(timer.deadline, timer.id);
    std::stable_sort(due.begin(), due.end());
    interp.timers.erase(std::remove_if(interp.timers.begin(), interp.timers.end(), [](const Interpreter::Timer &timer) { return timer.canceled; }), interp.timers.end());
    for (const auto &entry : due) {
        auto find = [&interp, id = entry.second]() { return std::find_if(interp.timers.begin(), interp.timers.end(), [id](const Interpreter::Timer &timer) { return timer.id == id; }); };
        auto it = find();
        if (it == interp.timers.end() || it->canceled) continue;
        ObjectPtr callback = it->callback;
        std::vector<Value> args = it->args;
        if (it->animation_frame) {
            const auto milliseconds = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            args.push_back(Value::Num(static_cast<double>(milliseconds) / 1000.0));
        }
        if (it->repeat) it->deadline = now + std::max(it->interval, std::chrono::milliseconds(4));
        else interp.timers.erase(it);
        if (ran_any) *ran_any = true;
        interp.steps = 0;
        Completion result = CallFunction(interp, callback, args);
        if (result.type == CompletionType::Throw) return result;
        Completion microtasks = DrainMicrotasks(interp);
        if (microtasks.type == CompletionType::Throw) return microtasks;
    }
    return Completion::Norm();
}

void SchedulePromiseReaction(Interpreter &interp, const ObjectPtr &promise,
                             const std::tuple<ObjectPtr, ObjectPtr, ObjectPtr> &reaction) {
    interp.microtasks.push_back(MakeNativeFn([&interp, promise, reaction](std::vector<Value> &, bool &, std::string &) {
        const ObjectPtr &handler = promise->promise_state == 1 ? std::get<0>(reaction) : std::get<1>(reaction);
        const ObjectPtr &next = std::get<2>(reaction);
        if (!handler) {
            SettlePromise(interp, next, promise->promise_state, promise->promise_value);
            return Value::Undef();
        }
        std::vector<Value> args{promise->promise_value};
        Completion called = CallFunction(interp, handler, args);
        if (called.type == CompletionType::Throw) SettlePromise(interp, next, 2, called.value);
        else SettlePromise(interp, next, 1, called.value);
        return Value::Undef();
    }).obj);
}

void SettlePromise(Interpreter &interp, const ObjectPtr &promise, int state, Value value) {
    if (!promise || !promise->is_promise || promise->promise_state != 0) return;
    // Adopting another promise preserves the one-way settlement invariant.
    if (state == 1 && value.type == VType::Object && value.obj && value.obj->is_promise) {
        ObjectPtr adopted = value.obj;
        if (adopted == promise) { SettlePromise(interp, promise, 2, Value::Str("promise resolved with itself")); return; }
        auto forward = [&, promise, adopted]() {
            if (adopted->promise_state) SettlePromise(interp, promise, adopted->promise_state, adopted->promise_value);
            else adopted->promise_reactions.emplace_back(nullptr, nullptr, promise);
        };
        forward();
        return;
    }
    promise->promise_state = state;
    promise->promise_value = std::move(value);
    for (const auto &reaction : promise->promise_reactions) SchedulePromiseReaction(interp, promise, reaction);
    promise->promise_reactions.clear();
}

// Returns false (with `out` set to a Throw completion) if `target` isn't
// something assignable to (a bare identifier, a.b, or a[expr]) -- every
// caller propagates that the same way any other abrupt completion is
// propagated.
/**
 * @brief Assigns a value to an assignment target: a bare identifier (updating an existing binding or creating a global) or a member expression (a.b / a[expr], via SetProp).
 * @param interp The interpreter, used to evaluate a computed target's object/key subexpressions.
 * @param target The assignment target node (must be an Ident or Member node).
 * @param val The value to assign.
 * @param env The scope to resolve identifiers and evaluate subexpressions in.
 * @param out Set to the abrupt completion on failure (an evaluation error, a non-object member base, or an unassignable target kind); untouched on success.
 * @return true if the assignment succeeded, false otherwise (with `out` set).
 */
bool AssignTo(Interpreter &interp, const Node &target, Value val, EnvPtr &env, Completion &out) {
    if (target.kind == NodeKind::Ident) {
        Value *slot = env->Find(target.name);
        if (slot) {
            *slot = std::move(val);
        } else {
            interp.global->Define(target.name, std::move(val));  // undeclared assignment creates a global, matching non-strict-mode JS
        }
        return true;
    }
    if (target.kind == NodeKind::Member) {
        Completion objc = EvalExpr(interp, *target.a, env);
        if (objc.IsAbrupt()) {
            out = objc;
            return false;
        }
        if (objc.value.type != VType::Object || !objc.value.obj) {
            out = Completion::Thr("cannot set property '" + (target.computed ? std::string("[...]") : target.prop_name) + "' of " + ToDisplayString(objc.value) + " (" + DescribeExpr(*target.a) + ")");
            return false;
        }
        std::string key = target.prop_name;
            if (target.computed) {
            Completion keyc = EvalExpr(interp, *target.b, env);
            if (keyc.IsAbrupt()) {
                out = keyc;
                return false;
            }
                key = PropertyKey(keyc.value);
            }
            if (objc.value.obj->proxy_target) {
                Value trap = objc.value.obj->proxy_handler ? GetProp(objc.value.obj->proxy_handler, "set") : Value::Undef();
                if (trap.type == VType::Object && trap.obj && trap.obj->is_function) {
                    std::vector<Value> trap_args{Value::Obj(objc.value.obj->proxy_target), Value::Str(key), val, objc.value};
                    Completion called = CallFunction(interp, trap.obj, trap_args);
                    if (called.IsAbrupt()) { out = called; return false; }
                    return true;
                }
                objc.value.obj = objc.value.obj->proxy_target;
            }
            if (ObjectPtr setter = FindAccessor(objc.value.obj, key, true)) {
                std::vector<Value> args{val};
                Completion called = CallFunction(interp, setter, args, &objc.value);
                if (called.IsAbrupt()) { out = called; return false; }
                return true;
            }
            if (objc.value.obj == interp.window_object && key != "location") {  // `window.location = url` navigates (SetProp)
                auto global_slot = interp.global->vars.find(key);
                if (global_slot != interp.global->vars.end()) global_slot->second = val;
            }
            SetProp(objc.value.obj, key, val);
        return true;
    }
    out = Completion::Thr("invalid assignment target");
    return false;
}

// The parser represents a destructuring assignment's left side with its
// ordinary array/object literal nodes.  Interpret those nodes as assignment
// patterns here, preserving member targets and defaults without introducing a
// second expression grammar just for the assignment form.
Completion AssignPattern(Interpreter &interp, const Node &target, Value value, EnvPtr &env) {
    const Node *destination = &target;
    if (target.kind == NodeKind::Assign && target.op == "=") {
        if (value.type == VType::Undefined) {
            Completion fallback = EvalExpr(interp, *target.b, env);
            if (fallback.IsAbrupt()) return fallback;
            value = fallback.value;
        }
        destination = target.a.get();
    }
    if (destination->kind == NodeKind::ArrayLit) {
        if (value.type != VType::Object || !value.obj || !value.obj->is_array)
            return Completion::Thr("array destructuring requires an array");
        for (size_t i = 0; i < destination->elements.size(); ++i) {
            if (!destination->elements[i]) continue;
            Completion assigned = AssignPattern(interp, *destination->elements[i], GetProp(value.obj, std::to_string(i)), env);
            if (assigned.IsAbrupt()) return assigned;
        }
        return Completion::Norm();
    }
    if (destination->kind == NodeKind::ObjectLit) {
        if (value.type != VType::Object || !value.obj)
            return Completion::Thr("object destructuring requires an object");
        for (const auto &property : destination->obj_props) {
            Completion assigned = AssignPattern(interp, *property.second, GetProp(value.obj, property.first), env);
            if (assigned.IsAbrupt()) return assigned;
        }
        return Completion::Norm();
    }
    Completion out;
    if (!AssignTo(interp, *destination, value, env, out)) return out;
    return Completion::Norm();
}

/**
 * @brief Evaluates an expression AST node to a value, dispatching on the node's kind (literals, identifiers, unary/update/binary/logical/assignment operators, member access, calls, the conditional operator, and function expressions).
 * @param interp The interpreter, providing step-counting/call-depth state and consulted for the per-evaluation step guard.
 * @param n The expression node to evaluate.
 * @param env The scope to resolve identifiers and evaluate subexpressions in.
 * @return Normal with the expression's value, or an abrupt completion (Throw for an evaluation error, or whatever a nested call/member/assignment propagated).
 */
// Date.prototype methods, bound to their receiver (natives get no `this`).
std::string DateToIso(double ms) {
    if (std::isnan(ms)) return "Invalid Date";
    const std::time_t seconds = static_cast<std::time_t>(std::floor(ms / 1000.0));
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms - std::floor(ms / 1000.0) * 1000.0));
    return buf;
}
bool DateMember(const ObjectPtr &date, const std::string &key, Value &out) {
    static const std::unordered_map<std::string, int> kFields = {
        {"getFullYear", 0}, {"getMonth", 1}, {"getDate", 2}, {"getDay", 3}, {"getHours", 4}, {"getMinutes", 5},
        {"getSeconds", 6}, {"getMilliseconds", 7}, {"getTimezoneOffset", 8}, {"getTime", 9}, {"valueOf", 9},
        {"toISOString", 10}, {"toJSON", 10}, {"toString", 11}, {"toLocaleString", 11}, {"toLocaleDateString", 12},
        {"toDateString", 12}, {"toLocaleTimeString", 13}, {"toTimeString", 13}, {"setTime", 14}};
    auto it = kFields.find(key);
    if (it == kFields.end()) return false;
    const int which = it->second;
    out = MakeNativeFn([date, which](const std::vector<Value> &args, bool &, std::string &) {
        const double ms = date->date_ms;
        if (which == 14) { date->date_ms = args.empty() ? std::nan("") : ToNumber(args[0]); return Value::Num(date->date_ms); }
        if (which == 9) return Value::Num(ms);
        if (which == 10) return Value::Str(DateToIso(ms));
        if (std::isnan(ms)) return which >= 11 ? Value::Str("Invalid Date") : Value::Num(ms);
        const std::time_t seconds = static_cast<std::time_t>(std::floor(ms / 1000.0));
        std::tm tm{};
        localtime_r(&seconds, &tm);
        char buf[96];
        switch (which) {
            case 0: return Value::Num(tm.tm_year + 1900);
            case 1: return Value::Num(tm.tm_mon);
            case 2: return Value::Num(tm.tm_mday);
            case 3: return Value::Num(tm.tm_wday);
            case 4: return Value::Num(tm.tm_hour);
            case 5: return Value::Num(tm.tm_min);
            case 6: return Value::Num(tm.tm_sec);
            case 7: return Value::Num(ms - std::floor(ms / 1000.0) * 1000.0);
            case 8: return Value::Num(-static_cast<double>(tm.tm_gmtoff) / 60.0);
            case 12: std::strftime(buf, sizeof(buf), "%a %b %d %Y", &tm); return Value::Str(buf);
            case 13: std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm); return Value::Str(buf);
            default: std::strftime(buf, sizeof(buf), "%a %b %d %Y %H:%M:%S GMT%z", &tm); return Value::Str(buf);
        }
    });
    return true;
}

// Runs one event through capture -> target -> bubble over the node's
// ancestor chain (the window pseudo-target last), calling addEventListener
// listeners and on<type> property handlers. Returns !defaultPrevented.
Value DispatchDomEvent(Interpreter &interp, HtmlDoc *doc, DomNode *target, const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj) { threw = true; error = "dispatchEvent requires an Event"; return Value::Undef(); }
        ObjectPtr event = args[0].obj;
        auto state_for_target = GetDomEventState(*doc);
        Value type = GetProp(event, "type");
        if (type.type != VType::String || type.str.empty()) { threw = true; error = "event type is required"; return Value::Undef(); }
        event->props["target"] = target == &state_for_target->window_node ? Value::Obj(interp.window_object) : WrapDomNode(*doc, target);
        { Value event_target = event->props["target"]; event->props["srcElement"] = event_target; }
        event->props["defaultPrevented"] = Value::Bool(false);
        event->props["cancelBubble"] = Value::Bool(false);
        event->props["immediatePropagationStopped"] = Value::Bool(false);
        event->props["preventDefault"] = MakeNativeFn([event](const std::vector<Value> &, bool &, std::string &) { event->props["defaultPrevented"] = Value::Bool(true); return Value::Undef(); });
        event->props["stopPropagation"] = MakeNativeFn([event](const std::vector<Value> &, bool &, std::string &) { event->props["cancelBubble"] = Value::Bool(true); return Value::Undef(); });
        event->props["stopImmediatePropagation"] = MakeNativeFn([event](const std::vector<Value> &, bool &, std::string &) { event->props["cancelBubble"] = Value::Bool(true); event->props["immediatePropagationStopped"] = Value::Bool(true); return Value::Undef(); });
        if (type.str == "mouseover" || type.str == "mouseenter") target->interaction_hover = true;
        else if (type.str == "mouseout" || type.str == "mouseleave") target->interaction_hover = false;
        else if (type.str == "mousedown") {
            target->interaction_active = true;
            if (target->tag == "input" || target->tag == "textarea" || target->tag == "select" || target->tag == "button") {
                std::function<void(DomNode *)> clear_focus = [&](DomNode *current) {
                    if (!current) return;
                    current->interaction_focus = false;
                    for (const auto &child : current->children) clear_focus(child.get());
                    if (current->shadow_root) clear_focus(current->shadow_root.get());
                };
                clear_focus(doc->root.get());
                target->interaction_focus = true;
            }
        }
        else if (type.str == "mouseup" || type.str == "click") target->interaction_active = false;
        std::vector<DomNode *> path;
        auto state = GetDomEventState(*doc);
        for (DomNode *node = target; node; node = node->parent) path.push_back(node);
        if (target != &state->window_node) path.push_back(&state->window_node);
        auto fire = [&](DomNode *node, bool capture) -> bool {
            event->props["currentTarget"] = node == &state->window_node ? Value::Obj(interp.window_object) : WrapDomNode(*doc, node);
            auto node_listeners = state->listeners.find(node);
            if (node_listeners != state->listeners.end()) {
                auto typed = node_listeners->second.find(type.str);
                if (typed != node_listeners->second.end()) for (size_t index = 0; index < typed->second.size();) {
                    DomEventListener listener = typed->second[index];
                    if (listener.capture != capture) { ++index; continue; }
                    std::vector<Value> event_args{Value::Obj(event)};
                    Value receiver = node == &state->window_node ? Value::Obj(interp.window_object) : WrapDomNode(*doc, node);
                    Completion called = CallFunction(interp, listener.callback, event_args, &receiver);
                    if (listener.once) typed->second.erase(typed->second.begin() + static_cast<std::ptrdiff_t>(index)); else ++index;
                    if (called.type == CompletionType::Throw) { threw = true; error = ToDisplayString(called.value); return false; }
                    if (GetProp(event, "immediatePropagationStopped").Truthy()) break;
                }
            }
            if (!capture && !GetProp(event, "immediatePropagationStopped").Truthy()) {
                auto handlers = state->property_handlers.find(node);
                if (handlers != state->property_handlers.end()) {
                    auto handler = handlers->second.find(type.str);
                    if (handler != handlers->second.end() && handler->second) {
                        std::vector<Value> event_args{Value::Obj(event)};
                        Value receiver = node == &state->window_node ? Value::Obj(interp.window_object) : WrapDomNode(*doc, node);
                        Completion called = CallFunction(interp, handler->second, event_args, &receiver);
                        if (called.type == CompletionType::Throw) { threw = true; error = ToDisplayString(called.value); return false; }
                    }
                }
            }
            return !GetProp(event, "cancelBubble").Truthy();
        };
        for (auto it = path.rbegin(); it != path.rend(); ++it) if (!fire(*it, true)) return Value::Bool(!GetProp(event, "defaultPrevented").Truthy());
        if (!fire(target, false)) return Value::Bool(!GetProp(event, "defaultPrevented").Truthy());
        if (GetProp(event, "bubbles").Truthy()) for (size_t index = 1; index < path.size(); ++index) if (!fire(path[index], false)) break;
        return Value::Bool(!GetProp(event, "defaultPrevented").Truthy());
}

// Source-ish text for an expression, for error messages ("a.b.c is not a function").
std::string SourceExcerpt(const Node &n) {
    if (!n.source) return "";
    const std::string &src = *n.source;
    const size_t at = std::min(static_cast<size_t>(std::max(n.pos, 0)), src.size());
    size_t line = 1;
    for (size_t k = 0; k < at; ++k) if (src[k] == '\n') ++line;
    std::string excerpt = src.substr(at > 60 ? at - 60 : 0, (at > 60 ? 60 : at) + 40);
    for (char &c : excerpt) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return " [line " + std::to_string(line) + ": `" + excerpt + "`]";
}

std::string DescribeExpr(const Node &n) {
    if (n.kind == NodeKind::Ident) return n.name;
    if (n.kind == NodeKind::Member && n.a) return DescribeExpr(*n.a) + (n.computed ? "[...]" : "." + n.prop_name);
    if (n.kind == NodeKind::Call && n.a) return DescribeExpr(*n.a) + "(...)";
    return "expression";
}

// ---- operator semantics shared by Binary and compound assignment -------
int32_t ToInt32(double d) {
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<int32_t>(static_cast<uint32_t>(m));
}
uint32_t ToUint32(double d) { return static_cast<uint32_t>(ToInt32(d)); }

// Abstract (==) equality: same-type falls back to ===, null == undefined,
// number/string/boolean compare numerically, an object against a
// primitive compares through its string form.
bool LooseEquals(const Value &a, const Value &b) {
    if (a.type == b.type) return StrictEquals(a, b);
    const bool a_nullish = a.type == VType::Null || a.type == VType::Undefined;
    const bool b_nullish = b.type == VType::Null || b.type == VType::Undefined;
    if (a_nullish || b_nullish) return a_nullish && b_nullish;
    if (a.type == VType::Object || b.type == VType::Object) {
        const Value &object = a.type == VType::Object ? a : b;
        const Value &primitive = a.type == VType::Object ? b : a;
        if (primitive.type == VType::Number) return ToNumber(object) == primitive.num;
        return ToDisplayString(object) == ToDisplayString(primitive);
    }
    return ToNumber(a) == ToNumber(b);
}

// Whether `key` is reachable on `obj` (own data/accessor, or up the prototype chain).
bool HasProperty(const ObjectPtr &obj, const std::string &key) {
    for (ObjectPtr cur = obj; cur; cur = cur->prototype) {
        if (cur->props.count(key) || cur->getters.count(key) || cur->setters.count(key)) return true;
    }
    return false;
}

// `value instanceof ctor`: ctor's prototype object somewhere on value's chain.
// A class keeps its instances' prototype in its own [[Prototype]] slot (see
// NodeKind::New); an ordinary function exposes it as the `prototype` property.
bool InstanceOf(const Value &value, const Value &ctor) {
    if (value.type != VType::Object || !value.obj || ctor.type != VType::Object || !ctor.obj) return false;
    ObjectPtr target;
    if (ctor.obj->is_class) target = ctor.obj->prototype;
    else {
        Value proto = GetProp(ctor.obj, "prototype");
        if (proto.type == VType::Object) target = proto.obj;
    }
    if (!target) return false;
    for (ObjectPtr cur = value.obj->prototype; cur; cur = cur->prototype) {
        if (cur == target) return true;
    }
    return false;
}

Completion ApplyBinary(const std::string &op, const Value &lv, const Value &rv) {
    if (op == "+") {
        if (lv.type == VType::String || rv.type == VType::String || lv.type == VType::Object || rv.type == VType::Object) {
            return Completion::Norm(Value::Str(ToDisplayString(lv) + ToDisplayString(rv)));
        }
        return Completion::Norm(Value::Num(ToNumber(lv) + ToNumber(rv)));
    }
    if (op == "-") return Completion::Norm(Value::Num(ToNumber(lv) - ToNumber(rv)));
    if (op == "*") return Completion::Norm(Value::Num(ToNumber(lv) * ToNumber(rv)));
    if (op == "/") return Completion::Norm(Value::Num(ToNumber(lv) / ToNumber(rv)));
    if (op == "%") return Completion::Norm(Value::Num(std::fmod(ToNumber(lv), ToNumber(rv))));
    if (op == "**") return Completion::Norm(Value::Num(std::pow(ToNumber(lv), ToNumber(rv))));
    if (op == "===") return Completion::Norm(Value::Bool(StrictEquals(lv, rv)));
    if (op == "!==") return Completion::Norm(Value::Bool(!StrictEquals(lv, rv)));
    if (op == "==") return Completion::Norm(Value::Bool(LooseEquals(lv, rv)));
    if (op == "!=") return Completion::Norm(Value::Bool(!LooseEquals(lv, rv)));
    if (op == "&") return Completion::Norm(Value::Num(ToInt32(ToNumber(lv)) & ToInt32(ToNumber(rv))));
    if (op == "|") return Completion::Norm(Value::Num(ToInt32(ToNumber(lv)) | ToInt32(ToNumber(rv))));
    if (op == "^") return Completion::Norm(Value::Num(ToInt32(ToNumber(lv)) ^ ToInt32(ToNumber(rv))));
    if (op == "<<") return Completion::Norm(Value::Num(static_cast<int32_t>(ToUint32(ToNumber(lv)) << (ToUint32(ToNumber(rv)) & 31u))));
    if (op == ">>") return Completion::Norm(Value::Num(ToInt32(ToNumber(lv)) >> (ToUint32(ToNumber(rv)) & 31u)));
    if (op == ">>>") return Completion::Norm(Value::Num(ToUint32(ToNumber(lv)) >> (ToUint32(ToNumber(rv)) & 31u)));
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        bool result;
        if (lv.type == VType::String && rv.type == VType::String) {
            result = op == "<" ? lv.str < rv.str : op == ">" ? lv.str > rv.str : op == "<=" ? lv.str <= rv.str : lv.str >= rv.str;
        } else {
            double a = ToNumber(lv), b = ToNumber(rv);
            result = op == "<" ? a < b : op == ">" ? a > b : op == "<=" ? a <= b : a >= b;
        }
        return Completion::Norm(Value::Bool(result));
    }
    if (op == "instanceof") return Completion::Norm(Value::Bool(InstanceOf(lv, rv)));
    if (op == "in") {
        if (rv.type != VType::Object || !rv.obj) return Completion::Thr("right-hand side of 'in' is not an object");
        const std::string key = PropertyKey(lv);
        // Event handler properties exist (as null) on every event target:
        // frameworks probe `'oninput' in document` to detect event support.
        if ((rv.obj->dom_node || rv.obj->is_document || rv.obj->is_window) && key.size() > 2 && key[0] == 'o' && key[1] == 'n') return Completion::Norm(Value::Bool(true));
        if (rv.obj->is_array) {
            long index = 0;
            if (IsArrayIndexKey(key, index)) return Completion::Norm(Value::Bool(rv.obj->props.count(key) > 0));
        }
        return Completion::Norm(Value::Bool(HasProperty(rv.obj, key) || GetProp(rv.obj, key).type != VType::Undefined));
    }
    return Completion::Thr("unsupported binary operator '" + op + "'");
}

// `var` names a function body (or program) declares, not descending into
// nested functions/classes -- cached on the owning node.
void CollectVarNames(const Node &n, std::vector<std::string> &out);
void CollectPatternNames(const BindingPattern &pattern, std::vector<std::string> &out) {
    if (pattern.kind == BindingPattern::Kind::Ident) { out.push_back(pattern.name); return; }
    for (const auto &element : pattern.elements) if (element) CollectPatternNames(*element, out);
    for (const auto &property : pattern.properties) if (property.second) CollectPatternNames(*property.second, out);
    if (pattern.rest) CollectPatternNames(*pattern.rest, out);
}
void CollectVarNames(const Node &n, std::vector<std::string> &out) {
    if (n.kind == NodeKind::FunctionDecl || n.kind == NodeKind::FunctionExpr || n.kind == NodeKind::Class) return;
    if (n.kind == NodeKind::VarDecl && n.boolean) {
        for (const auto &d : n.declarators) out.push_back(d.first);
        for (const auto &d : n.pattern_declarators) if (d.first) CollectPatternNames(*d.first, out);
    }
    if (n.kind == NodeKind::For && n.boolean && !n.name.empty()) out.push_back(n.name);
    for (const auto &child : n.body) if (child) CollectVarNames(*child, out);
    for (const Node *child : {n.init.get(), n.then_branch.get(), n.else_branch.get(), n.c.get()}) if (child) CollectVarNames(*child, out);
    for (const auto &entry : n.switch_cases) for (const auto &child : entry.second) if (child) CollectVarNames(*child, out);
}
void HoistVars(const Node &owner, const std::vector<NodePtr> &body, Environment &scope) {
    if (!owner.hoisted_computed) {
        for (const auto &stmt : body) if (stmt) CollectVarNames(*stmt, owner.hoisted_vars);
        owner.hoisted_computed = true;
    }
    for (const std::string &name : owner.hoisted_vars) {
        if (!scope.vars.count(name)) scope.vars[name] = Value::Undef();
    }
}

void JsonQuote(const std::string &text, std::string &out) {
    out += '"';
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out += raw;
        }
    }
    out += '"';
}

// JSON.stringify: own enumerable keys in order, toJSON() honoured,
// functions/undefined/symbols dropped (null inside arrays), optional indent.
// Returns false when the value has no JSON form at all.
bool JsonStringify(Interpreter &interp, const Value &input, const std::string &indent, const std::string &current, std::string &out, bool &threw, std::string &error, int depth) {
    if (depth > 200) { threw = true; error = "Converting circular structure to JSON"; return false; }
    Value value = input;
    if (value.type == VType::Object && value.obj && !value.obj->is_function) {
        Completion to_json = MemberGet(interp, value, "toJSON");
        if (!to_json.IsAbrupt() && to_json.value.type == VType::Object && to_json.value.obj && to_json.value.obj->is_function) {
            std::vector<Value> no_args;
            Completion converted = CallFunction(interp, to_json.value.obj, no_args, &value);
            if (converted.IsAbrupt()) { threw = true; error = ToDisplayString(converted.value); return false; }
            value = converted.value;
        }
    }
    switch (value.type) {
        case VType::Undefined: return false;
        case VType::Null: out += "null"; return true;
        case VType::Boolean: out += value.boolean ? "true" : "false"; return true;
        case VType::Number: out += std::isfinite(value.num) ? NumberToString(value.num) : "null"; return true;
        case VType::String: JsonQuote(value.str, out); return true;
        case VType::Object: break;
    }
    if (!value.obj || value.obj->is_function || value.obj->is_symbol) return false;
    const std::string inner = current + indent;
    const std::string separator = indent.empty() ? "," : ",\n" + inner;
    bool first = true;
    if (value.obj->is_array) {
        out += '[';
        for (long i = 0; i < ArrayLength(value.obj); ++i) {
            out += first ? (indent.empty() ? "" : "\n" + inner) : separator;
            first = false;
            if (!JsonStringify(interp, GetProp(value.obj, std::to_string(i)), indent, inner, out, threw, error, depth + 1)) { if (threw) return false; out += "null"; }
        }
        if (!first && !indent.empty()) out += "\n" + current;
        out += ']';
        return true;
    }
    out += '{';
    for (const std::string &key : EnumerableKeys(value.obj)) {
        std::string member;
        if (!JsonStringify(interp, GetProp(value.obj, key), indent, inner, member, threw, error, depth + 1)) { if (threw) return false; continue; }
        out += first ? (indent.empty() ? "" : "\n" + inner) : separator;
        first = false;
        JsonQuote(key, out);
        out += indent.empty() ? ":" : ": ";
        out += member;
    }
    if (!first && !indent.empty()) out += "\n" + current;
    out += '}';
    return true;
}

// Drains anything iterable into a list: arrays, strings, Map/Set, iterator
// objects, and objects with a [Symbol.iterator]() method.
bool IterateValues(Interpreter &interp, const Value &source, std::vector<Value> &out, std::string &error) {
    if (source.type == VType::String) {
        for (size_t i = 0; i < source.str.size();) {
            const unsigned char lead = static_cast<unsigned char>(source.str[i]);
            const size_t width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            out.push_back(Value::Str(source.str.substr(i, width)));
            i += width;
        }
        return true;
    }
    if (source.type != VType::Object || !source.obj) { error = ToDisplayString(source) + " is not iterable"; return false; }
    const ObjectPtr &obj = source.obj;
    if (obj->is_array) {
        const long length = ArrayLength(obj);
        for (long i = 0; i < length; ++i) out.push_back(GetProp(obj, std::to_string(i)));
        return true;
    }
    if (obj->is_set) { for (const auto &entry : obj->collection_entries) out.push_back(entry.first); return true; }
    if (obj->is_map) { for (const auto &entry : obj->collection_entries) out.push_back(MakeArray({entry.first, entry.second})); return true; }
    Value iterator = source;
    if (!obj->is_list_iterator) {
        Completion factory = MemberGet(interp, source, "@@iterator");
        if (factory.IsAbrupt()) { error = ToDisplayString(factory.value); return false; }
        if (factory.value.type == VType::Object && factory.value.obj && factory.value.obj->is_function) {
            std::vector<Value> no_args;
            Completion made = CallFunction(interp, factory.value.obj, no_args, &source);
            if (made.IsAbrupt()) { error = ToDisplayString(made.value); return false; }
            iterator = made.value;
        }
    }
    if (iterator.type != VType::Object || !iterator.obj) { error = "object is not iterable"; return false; }
    Value next = GetProp(iterator.obj, "next");
    if (next.type != VType::Object || !next.obj || !next.obj->is_function) { error = "object is not iterable"; return false; }
    for (long guard = 0; guard < 10'000'000; ++guard) {
        std::vector<Value> no_args;
        Completion step = CallFunction(interp, next.obj, no_args, &iterator);
        if (step.IsAbrupt()) { error = ToDisplayString(step.value); return false; }
        if (step.value.type != VType::Object || !step.value.obj) { error = "iterator result is not an object"; return false; }
        if (GetProp(step.value.obj, "done").Truthy()) return true;
        out.push_back(GetProp(step.value.obj, "value"));
    }
    error = "iterator did not finish";
    return false;
}

// `key` on the builtin constructor's prototype object (String.prototype.x, ...).
Value BuiltinProtoLookup(Interpreter &interp, const char *ctor_name, const std::string &key) {
    Value *ctor = interp.global->Find(ctor_name);
    if (!ctor || ctor->type != VType::Object || !ctor->obj) return Value::Undef();
    auto proto = ctor->obj->props.find("prototype");
    if (proto == ctor->obj->props.end() || proto->second.type != VType::Object || !proto->second.obj) return Value::Undef();
    return GetProp(proto->second.obj, key);
}

// X.prototype itself, as a value (null when X isn't set up).
Value BuiltinProtoLookupObject(Interpreter &interp, const char *ctor_name) {
    Value *ctor = interp.global->Find(ctor_name);
    if (!ctor || ctor->type != VType::Object || !ctor->obj) return Value::MakeNull();
    auto proto = ctor->obj->props.find("prototype");
    return proto == ctor->obj->props.end() ? Value::MakeNull() : proto->second;
}

// Property read on any value -- primitives' builtin methods, arrays,
// promises, DOM wrappers, plain objects -- shared by the Member expression
// and by the X.prototype method shims (which re-enter it with `this`).
Completion MemberGet(Interpreter &interp, const Value &base, const std::string &key) {
    Completion objc = Completion::Norm(base);
    if (objc.value.type == VType::Number) {
        const double source = objc.value.num;
        if (key == "toString") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) {
            const int radix = args.empty() || args[0].type == VType::Undefined ? 10 : static_cast<int>(ToNumber(args[0]));
            if (radix == 10 || radix < 2 || radix > 36 || !std::isfinite(source)) return Value::Str(NumberToString(source));
            const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
            double whole = std::floor(std::fabs(source)), fraction = std::fabs(source) - whole;
            std::string text;
            do { text.insert(text.begin(), digits[static_cast<int>(std::fmod(whole, radix))]); whole = std::floor(whole / radix); } while (whole >= 1);
            if (fraction > 0) {
                text += '.';
                for (int i = 0; i < 12 && fraction > 0; ++i) { fraction *= radix; const int digit = static_cast<int>(fraction); text += digits[digit]; fraction -= digit; }
            }
            return Value::Str((source < 0 ? "-" : "") + text);
        }));
        if (key == "valueOf") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Num(source); }));
        if (key == "toLocaleString") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Str(NumberToString(source)); }));
        if (key == "toFixed") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { int digits = args.empty() ? 0 : std::max(0, std::min(100, static_cast<int>(ToNumber(args[0])))); std::ostringstream out; out << std::fixed << std::setprecision(digits) << source; return Value::Str(out.str()); }));
        if (key == "toPrecision") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { if (args.empty()) return Value::Str(NumberToString(source)); int digits = std::max(1, std::min(100, static_cast<int>(ToNumber(args[0])))); std::ostringstream out; out << std::setprecision(digits) << source; return Value::Str(out.str()); }));
        {
            Value inherited = BuiltinProtoLookup(interp, "Number", key);
            if (inherited.type == VType::Object && inherited.obj && inherited.obj->is_function) { interp.member_receiver = objc.value; interp.member_receiver_fn = inherited.obj.get(); }
            return Completion::Norm(inherited);
        }
    }
    if (objc.value.type == VType::Boolean) {
        const bool source = objc.value.boolean;
        if (key == "toString") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Str(source ? "true" : "false"); }));
        if (key == "valueOf") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Bool(source); }));
        return Completion::Norm(Value::Undef());
    }
    if (objc.value.type == VType::String) {
        const std::string source = objc.value.str;
        if (key == "length") return Completion::Norm(Value::Num(static_cast<double>(source.size())));
        {
            long char_index = 0;
            if (IsArrayIndexKey(key, char_index)) {
                return Completion::Norm(static_cast<size_t>(char_index) < source.size() ? Value::Str(source.substr(static_cast<size_t>(char_index), 1)) : Value::Undef());
            }
        }
        if (key == "indexOf" || key == "lastIndexOf") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) {
            const std::string needle = args.empty() ? "undefined" : ToDisplayString(args[0]);
            size_t at;
            if (key == "indexOf") {
                const double from = args.size() > 1 ? ToNumber(args[1]) : 0;
                at = source.find(needle, from > 0 ? static_cast<size_t>(from) : 0);
            } else {
                const double from = args.size() > 1 && !std::isnan(ToNumber(args[1])) ? ToNumber(args[1]) : static_cast<double>(source.size());
                at = source.rfind(needle, from > 0 ? static_cast<size_t>(from) : 0);
            }
            return Value::Num(at == std::string::npos ? -1.0 : static_cast<double>(at));
        }));
        if (key == "charAt" || key == "charCodeAt" || key == "codePointAt" || key == "at") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) {
            double wanted = args.empty() ? 0 : ToNumber(args[0]);
            if (std::isnan(wanted)) wanted = 0;
            if (key == "at" && wanted < 0) wanted += static_cast<double>(source.size());
            const bool inside = wanted >= 0 && wanted < static_cast<double>(source.size());
            if (key == "charAt") return Value::Str(inside ? source.substr(static_cast<size_t>(wanted), 1) : "");
            if (key == "at") return inside ? Value::Str(source.substr(static_cast<size_t>(wanted), 1)) : Value::Undef();
            if (!inside) return key == "charCodeAt" ? Value::Num(std::nan("")) : Value::Undef();
            // Decode the UTF-8 sequence that starts here into its code point.
            const size_t i = static_cast<size_t>(wanted);
            const unsigned char lead = static_cast<unsigned char>(source[i]);
            uint32_t cp = lead;
            const size_t width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (width > 1 && i + width <= source.size()) {
                cp = lead & (0xFFu >> (width + 1));
                for (size_t k = 1; k < width; ++k) cp = (cp << 6) | (static_cast<unsigned char>(source[i + k]) & 0x3Fu);
            }
            return Value::Num(static_cast<double>(cp));
        }));
        if (key == "substr") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) {
            double start = args.empty() ? 0 : ToNumber(args[0]);
            const double size = static_cast<double>(source.size());
            if (start < 0) start = std::max(0.0, size + start);
            if (start >= size) return Value::Str("");
            const double count = args.size() > 1 && args[1].type != VType::Undefined ? std::max(0.0, ToNumber(args[1])) : size - start;
            return Value::Str(source.substr(static_cast<size_t>(start), static_cast<size_t>(std::min(count, size - start))));
        }));
        if (key == "concat") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) {
            std::string out = source;
            for (const Value &arg : args) out += ToDisplayString(arg);
            return Value::Str(out);
        }));
        if (key == "trimStart" || key == "trimEnd" || key == "trimLeft" || key == "trimRight") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &, bool &, std::string &) {
            size_t begin = 0, end = source.size();
            if (key == "trimStart" || key == "trimLeft") while (begin < end && std::isspace(static_cast<unsigned char>(source[begin]))) ++begin;
            else while (end > begin && std::isspace(static_cast<unsigned char>(source[end - 1]))) --end;
            return Value::Str(source.substr(begin, end - begin));
        }));
        if (key == "localeCompare") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) {
            const int order = source.compare(args.empty() ? "undefined" : ToDisplayString(args[0]));
            return Value::Num(order < 0 ? -1 : order > 0 ? 1 : 0);
        }));
        if (key == "toString" || key == "valueOf" || key == "normalize" || key == "toLocaleLowerCase" || key == "toLocaleUpperCase") {
            if (key == "toString" || key == "valueOf" || key == "normalize") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Str(source); }));
            return MemberGet(interp, base, key == "toLocaleLowerCase" ? "toLowerCase" : "toUpperCase");
        }
        if (key == "@@iterator") return Completion::Norm(MakeNativeFn([&interp, source](const std::vector<Value> &, bool &, std::string &) {
            std::vector<Value> chars;
            std::string ignored;
            IterateValues(interp, Value::Str(source), chars, ignored);
            return MakeListIterator(std::move(chars));
        }));
        auto make_array = [](const std::vector<std::string> &parts) {
            auto result = std::make_shared<ObjectData>(); result->is_array = true;
            for (size_t i = 0; i < parts.size(); ++i) result->props[std::to_string(i)] = Value::Str(parts[i]);
            result->props["length"] = Value::Num(static_cast<double>(parts.size())); return Value::Obj(result);
        };
        if (key == "toUpperCase") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { std::string out = source; for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return Value::Str(out); }));
        if (key == "toLowerCase") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { std::string out = source; for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return Value::Str(out); }));
        if (key == "includes" || key == "startsWith" || key == "endsWith") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) { std::string needle = args.empty() ? "undefined" : ToDisplayString(args[0]); if (key == "includes") return Value::Bool(source.find(needle) != std::string::npos); if (key == "startsWith") return Value::Bool(source.rfind(needle, 0) == 0); return Value::Bool(source.size() >= needle.size() && source.compare(source.size() - needle.size(), needle.size(), needle) == 0); }));
        if (key == "slice" || key == "substring") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) { long start = args.empty() ? 0 : static_cast<long>(ToNumber(args[0])), end = args.size() < 2 ? static_cast<long>(source.size()) : static_cast<long>(ToNumber(args[1])); if (key == "slice") { if (start < 0) start = std::max(0L, static_cast<long>(source.size()) + start); if (end < 0) end = std::max(0L, static_cast<long>(source.size()) + end); } else { start = std::max(0L, start); end = std::max(0L, end); if (start > end) std::swap(start, end); } start = std::min(start, static_cast<long>(source.size())); end = std::min(end, static_cast<long>(source.size())); return Value::Str(source.substr(static_cast<size_t>(start), static_cast<size_t>(std::max(0L, end - start)))); }));
        if (key == "trim") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { size_t begin = 0, end = source.size(); while (begin < end && std::isspace(static_cast<unsigned char>(source[begin]))) ++begin; while (end > begin && std::isspace(static_cast<unsigned char>(source[end - 1]))) --end; return Value::Str(source.substr(begin, end - begin)); }));
        if (key == "repeat") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { long count = args.empty() ? 0 : std::max(0L, static_cast<long>(ToNumber(args[0]))); std::string out; for (long i = 0; i < count; ++i) out += source; return Value::Str(out); }));
        if (key == "padStart" || key == "padEnd") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) { long width = args.empty() ? 0 : static_cast<long>(ToNumber(args[0])); std::string fill = args.size() < 2 ? " " : ToDisplayString(args[1]); if (width <= static_cast<long>(source.size()) || fill.empty()) return Value::Str(source); std::string padding; while (static_cast<long>(padding.size()) < width - static_cast<long>(source.size())) padding += fill; padding.resize(static_cast<size_t>(width - static_cast<long>(source.size()))); return Value::Str(key == "padStart" ? padding + source : source + padding); }));
        if (key == "split") return Completion::Norm(MakeNativeFn([source, make_array](const std::vector<Value> &args, bool &, std::string &) { if (args.empty()) return make_array({source}); std::string separator = ToDisplayString(args[0]); std::vector<std::string> parts; if (separator.empty()) for (char c : source) parts.emplace_back(1, c); else { size_t from = 0, at; while ((at = source.find(separator, from)) != std::string::npos) { parts.push_back(source.substr(from, at - from)); from = at + separator.size(); } parts.push_back(source.substr(from)); } return make_array(parts); }));
        if (key == "replace" || key == "replaceAll") return Completion::Norm(MakeNativeFn([&interp, source, key](const std::vector<Value> &args, bool &threw, std::string &err) {
            if (args.size() < 2) return Value::Str(source);
            const bool by_regex = args[0].type == VType::Object && args[0].obj && args[0].obj->is_regexp && args[0].obj->regexp;
            const bool all = key == "replaceAll" || (by_regex && args[0].obj->regexp_global);
            const ObjectPtr callback = (args[1].type == VType::Object && args[1].obj && args[1].obj->is_function) ? args[1].obj : nullptr;
            const std::string needle = by_regex ? "" : ToDisplayString(args[0]);
            const std::string templ = callback ? "" : ToDisplayString(args[1]);
            std::string out;
            size_t pos = 0;
            while (pos <= source.size()) {
                std::vector<std::pair<int, int>> groups;
                if (by_regex) {
                    mep_regex::Match found = args[0].obj->regexp->Search(source, static_cast<int>(pos));
                    if (!found.ok()) break;
                    groups.assign(found.groups.begin(), found.groups.end());
                } else {
                    const size_t at = source.find(needle, pos);
                    if (at == std::string::npos) break;
                    groups.emplace_back(static_cast<int>(at), static_cast<int>(at + needle.size()));
                }
                const size_t start = static_cast<size_t>(groups[0].first), end = static_cast<size_t>(groups[0].second);
                auto group_text = [&](size_t g) { return g < groups.size() && groups[g].first >= 0 ? source.substr(static_cast<size_t>(groups[g].first), static_cast<size_t>(groups[g].second - groups[g].first)) : std::string(); };
                out += source.substr(pos, start - pos);
                if (callback) {
                    std::vector<Value> call_args;
                    for (size_t g = 0; g < groups.size(); ++g) call_args.push_back(groups[g].first < 0 ? Value::Undef() : Value::Str(group_text(g)));
                    call_args.push_back(Value::Num(static_cast<double>(start)));
                    call_args.push_back(Value::Str(source));
                    Completion called = CallFunction(interp, callback, call_args);
                    if (called.IsAbrupt()) { threw = true; err = ToDisplayString(called.value); return Value::Undef(); }
                    out += ToDisplayString(called.value);
                } else {
                    for (size_t i = 0; i < templ.size(); ++i) {
                        if (templ[i] == '$' && i + 1 < templ.size()) {
                            const char next = templ[i + 1];
                            if (next == '$') { out += '$'; ++i; continue; }
                            if (next == '&') { out += group_text(0); ++i; continue; }
                            if (next >= '1' && next <= '9' && static_cast<size_t>(next - '0') < groups.size()) { out += group_text(static_cast<size_t>(next - '0')); ++i; continue; }
                        }
                        out += templ[i];
                    }
                }
                if (end == start) {  // empty match: copy one char and move on
                    if (start < source.size()) out += source[start];
                    pos = start + 1;
                } else pos = end;
                if (!all) break;
            }
            if (pos < source.size()) out += source.substr(pos);
            return Value::Str(out);
        }));
        if (key == "match" || key == "search") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) {
            const bool by_regex = !args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_regexp && args[0].obj->regexp;
            std::shared_ptr<mep_regex::Regex> regex = by_regex ? args[0].obj->regexp : std::make_shared<mep_regex::Regex>(args.empty() ? "" : ToDisplayString(args[0]), false);
            if (!regex->ok()) return Value::MakeNull();
            mep_regex::Match found = regex->Search(source);
            if (key == "search") return Value::Num(found.ok() ? found.groups[0].first : -1);
            if (!found.ok()) return Value::MakeNull();
            if (by_regex && !args[0].obj->regexp_global) {
                std::vector<Value> exec_args{Value::Str(source)};
                bool exec_threw = false;
                std::string exec_error;
                return GetProp(args[0].obj, "exec").obj->native(exec_args, exec_threw, exec_error);
            }
            auto result = std::make_shared<ObjectData>();
            result->is_array = true;
            if (by_regex && args[0].obj->regexp_global) {
                long count = 0;
                size_t pos = 0;
                while (found.ok()) {
                    const size_t start = static_cast<size_t>(found.groups[0].first), end = static_cast<size_t>(found.groups[0].second);
                    result->props[std::to_string(count++)] = Value::Str(source.substr(start, end - start));
                    pos = end == start ? end + 1 : end;
                    if (pos > source.size()) break;
                    found = regex->Search(source, static_cast<int>(pos));
                }
                result->props["length"] = Value::Num(static_cast<double>(count));
                return Value::Obj(result);
            }
            for (size_t i = 0; i < found.groups.size(); ++i) {
                const auto group = found.groups[i];
                result->props[std::to_string(i)] = group.first < 0 ? Value::Undef() : Value::Str(source.substr(static_cast<size_t>(group.first), static_cast<size_t>(group.second - group.first)));
            }
            result->props["length"] = Value::Num(static_cast<double>(found.groups.size()));
            result->props["index"] = Value::Num(found.groups[0].first);
            result->props["input"] = Value::Str(source);
            return Value::Obj(result);
        }));
        {
            Value inherited = BuiltinProtoLookup(interp, "String", key);
            if (inherited.type == VType::Object && inherited.obj && inherited.obj->is_function) { interp.member_receiver = objc.value; interp.member_receiver_fn = inherited.obj.get(); }
            return Completion::Norm(inherited);
        }
    }
    if (objc.value.type != VType::Object || !objc.value.obj) return Completion::Norm(Value::Undef());
    // Iteration surface of the builtin collections.
    if (objc.value.obj->is_array || objc.value.obj->is_map || objc.value.obj->is_set) {
        const ObjectPtr collection = objc.value.obj;
        const bool keyed = collection->is_map;
        if ((key == "@@iterator" || key == "values" || key == "keys" || key == "entries") && !collection->props.count(key)) {
            return Completion::Norm(MakeNativeFn([&interp, collection, key, keyed](const std::vector<Value> &, bool &threw, std::string &error) {
                std::vector<Value> items;
                if (!IterateValues(interp, Value::Obj(collection), items, error)) { threw = true; return Value::Undef(); }
                const bool want_entries = key == "entries" || (key == "@@iterator" && keyed);
                std::vector<Value> out;
                for (size_t i = 0; i < items.size(); ++i) {
                    if (collection->is_array) {
                        if (key == "keys") out.push_back(Value::Num(static_cast<double>(i)));
                        else if (want_entries) out.push_back(MakeArray({Value::Num(static_cast<double>(i)), items[i]}));
                        else out.push_back(items[i]);
                    } else if (keyed) {
                        if (want_entries) out.push_back(items[i]);
                        else out.push_back(GetProp(items[i].obj, key == "keys" ? "0" : "1"));
                    } else {
                        out.push_back(want_entries ? MakeArray({items[i], items[i]}) : items[i]);
                    }
                }
                return MakeListIterator(std::move(out));
            }));
        }
        if (collection->is_array && (key == "toString" || key == "toLocaleString") && !collection->props.count(key)) {
            return Completion::Norm(MakeNativeFn([collection](const std::vector<Value> &, bool &, std::string &) { return Value::Str(ToDisplayString(Value::Obj(collection))); }));
        }
        if (key == "forEach" && !collection->is_array) {
            return Completion::Norm(MakeNativeFn([&interp, collection](const std::vector<Value> &args, bool &threw, std::string &error) {
                if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) { threw = true; error = "forEach requires a callback"; return Value::Undef(); }
                const auto snapshot = collection->collection_entries;
                for (const auto &entry : snapshot) {
                    std::vector<Value> call_args{collection->is_map ? entry.second : entry.first, entry.first, Value::Obj(collection)};
                    Completion called = CallFunction(interp, args[0].obj, call_args);
                    if (called.IsAbrupt()) { threw = true; error = ToDisplayString(called.value); return Value::Undef(); }
                }
                return Value::Undef();
            }));
        }
    }
    if (objc.value.obj->proxy_target) {
        Value trap = objc.value.obj->proxy_handler ? GetProp(objc.value.obj->proxy_handler, "get") : Value::Undef();
        if (trap.type == VType::Object && trap.obj && trap.obj->is_function) {
            std::vector<Value> trap_args{Value::Obj(objc.value.obj->proxy_target), Value::Str(key), objc.value};
            return CallFunction(interp, trap.obj, trap_args);
        }
        objc.value.obj = objc.value.obj->proxy_target;
    }
    if (ObjectPtr getter = FindAccessor(objc.value.obj, key, false)) {
        std::vector<Value> args;
        return CallFunction(interp, getter, args, &objc.value);
    }
    if (objc.value.obj->dom_node && objc.value.obj->owner_doc && key == "dispatchEvent") {
        HtmlDoc *doc = objc.value.obj->owner_doc;
        DomNode *target = objc.value.obj->dom_node;
        return Completion::Norm(MakeNativeFn([&interp, doc, target](const std::vector<Value> &args, bool &threw, std::string &error) {
            return DispatchDomEvent(interp, doc, target, args, threw, error);
        }));
    }
    if (objc.value.obj->is_promise && (key == "then" || key == "catch" || key == "finally")) {
        ObjectPtr source = objc.value.obj;
        return Completion::Norm(MakeNativeFn([&interp, source, key](const std::vector<Value> &args, bool &, std::string &) {
            auto next = std::make_shared<ObjectData>();
            next->is_promise = true;
            ObjectPtr on_fulfilled, on_rejected;
            if (key == "then") {
                if (!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_function) on_fulfilled = args[0].obj;
                if (args.size() > 1 && args[1].type == VType::Object && args[1].obj && args[1].obj->is_function) on_rejected = args[1].obj;
            } else if (key == "catch") {
                if (!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_function) on_rejected = args[0].obj;
            } else {
                ObjectPtr callback = (!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_function) ? args[0].obj : nullptr;
                if (callback) {
                    on_fulfilled = MakeNativeFn([&interp, callback](const std::vector<Value> &values, bool &threw, std::string &error) {
                        std::vector<Value> no_args;
                        Completion finalizer = CallFunction(interp, callback, no_args);
                        if (finalizer.IsAbrupt()) { threw = true; error = ToDisplayString(finalizer.value); return Value::Undef(); }
                        return values.empty() ? Value::Undef() : values[0];
                    }).obj;
                    on_rejected = MakeNativeFn([&interp, callback](const std::vector<Value> &values, bool &threw, std::string &error) {
                        std::vector<Value> no_args;
                        Completion finalizer = CallFunction(interp, callback, no_args);
                        if (finalizer.IsAbrupt()) { threw = true; error = ToDisplayString(finalizer.value); return Value::Undef(); }
                        threw = true;
                        error = values.empty() ? "undefined" : ToDisplayString(values[0]);
                        return Value::Undef();
                    }).obj;
                }
            }
            auto reaction = std::make_tuple(on_fulfilled, on_rejected, next);
            if (source->promise_state) SchedulePromiseReaction(interp, source, reaction);
            else source->promise_reactions.push_back(std::move(reaction));
            return Value::Obj(next);
        }));
    }
    if (objc.value.obj == interp.window_object) {
        auto global_slot = interp.global->vars.find(key);
        if (global_slot != interp.global->vars.end()) return Completion::Norm(global_slot->second);
    }
    if (objc.value.obj->is_date) {
        Value date_method;
        if (DateMember(objc.value.obj, key, date_method)) return Completion::Norm(date_method);
    }
    Value ordinary = GetProp(objc.value.obj, key);
    if (ordinary.type != VType::Undefined) {
        // Hand back the function itself (identity and its own
        // properties must survive `obj.f === f`, `F.prototype`,
        // removeEventListener); a Call node picks the receiver up
        // from here instead of evaluating the base twice.
        if (ordinary.type == VType::Object && ordinary.obj && ordinary.obj->is_function) {
            interp.member_receiver = objc.value;
            interp.member_receiver_fn = ordinary.obj.get();
        }
        return Completion::Norm(ordinary);
    }
    if (objc.value.obj->is_array && (key == "map" || key == "filter" || key == "forEach" || key == "some" || key == "every" || key == "find" || key == "findIndex" || key == "reduce" || key == "flatMap")) {
        ObjectPtr source = objc.value.obj;
        return Completion::Norm(MakeNativeFn([&interp, source, key](const std::vector<Value> &args, bool &threw, std::string &err) {
            if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) { threw = true; err = key + " requires a callback"; return Value::Undef(); }
            ObjectPtr callback = args[0].obj;
            auto call = [&](const std::vector<Value> &values, Value &out) -> bool {
                std::vector<Value> mutable_values = values;
                Completion result = CallFunction(interp, callback, mutable_values);
                if (result.IsAbrupt()) { threw = true; err = result.label; return false; }
                out = result.value; return true;
            };
            const long length = ArrayLength(source);
            if (key == "reduce") {
                long index = 0;
                Value accumulator;
                if (args.size() > 1) accumulator = args[1];
                else { if (length == 0) { threw = true; err = "reduce of empty array"; return Value::Undef(); } accumulator = GetProp(source, "0"); index = 1; }
                for (; index < length; ++index) { Value next; if (!call({accumulator, GetProp(source, std::to_string(index)), Value::Num(static_cast<double>(index)), Value::Obj(source)}, next)) return Value::Undef(); accumulator = next; }
                return accumulator;
            }
            auto result = std::make_shared<ObjectData>(); result->is_array = true; long output_index = 0;
            for (long index = 0; index < length; ++index) {
                Value returned;
                if (!call({GetProp(source, std::to_string(index)), Value::Num(static_cast<double>(index)), Value::Obj(source)}, returned)) return Value::Undef();
                if (key == "forEach") continue;
                if (key == "some" && returned.Truthy()) return Value::Bool(true);
                if (key == "every" && !returned.Truthy()) return Value::Bool(false);
                if (key == "find" && returned.Truthy()) return GetProp(source, std::to_string(index));
                if (key == "findIndex" && returned.Truthy()) return Value::Num(static_cast<double>(index));
                if (key == "filter" && !returned.Truthy()) continue;
                if (key == "flatMap" && returned.type == VType::Object && returned.obj && returned.obj->is_array) {
                    for (long nested = 0; nested < ArrayLength(returned.obj); ++nested) result->props[std::to_string(output_index++)] = GetProp(returned.obj, std::to_string(nested));
                } else result->props[std::to_string(output_index++)] = key == "filter" ? GetProp(source, std::to_string(index)) : returned;
            }
            if (key == "forEach") return Value::Undef();
            if (key == "some") return Value::Bool(false);
            if (key == "every") return Value::Bool(true);
            if (key == "find") return Value::Undef();
            if (key == "findIndex") return Value::Num(-1);
            result->props["length"] = Value::Num(static_cast<double>(output_index));
            return Value::Obj(result);
        }));
    }
    if (objc.value.obj->is_function && (key == "call" || key == "apply" || key == "bind")) {
        ObjectPtr target = objc.value.obj;
        return Completion::Norm(MakeNativeFn([&interp, target, key](const std::vector<Value> &args, bool &threw, std::string &err) {
            Value receiver = args.empty() ? Value::Undef() : args[0];
            std::vector<Value> call_args;
            if (key == "apply") {
                if (args.size() > 1 && args[1].type != VType::Null && args[1].type != VType::Undefined) {
                    if (args[1].type != VType::Object || !args[1].obj || !args[1].obj->is_array) { threw = true; err = "apply requires an array"; return Value::Undef(); }
                    for (long i = 0; i < ArrayLength(args[1].obj); ++i) call_args.push_back(GetProp(args[1].obj, std::to_string(i)));
                }
            } else {
                for (size_t i = 1; i < args.size(); ++i) call_args.push_back(args[i]);
            }
            if (key == "bind") {
                return MakeNativeFn([&interp, target, receiver, call_args](const std::vector<Value> &later, bool &bound_threw, std::string &bound_err) mutable {
                    std::vector<Value> combined = call_args; combined.insert(combined.end(), later.begin(), later.end());
                    Completion called = CallFunction(interp, target, combined, &receiver);
                    if (called.IsAbrupt()) { bound_threw = true; bound_err = called.label; return Value::Undef(); }
                    return called.value;
                });
            }
            Completion called = CallFunction(interp, target, call_args, &receiver);
            if (called.IsAbrupt()) { threw = true; err = called.label; return Value::Undef(); }
            return called.value;
        }));
    }
    if (key == "valueOf") {
        Value target = objc.value;
        return Completion::Norm(MakeNativeFn([target](const std::vector<Value> &, bool &, std::string &) { return target; }));
    }
    {
        Value inherited;
        if (objc.value.obj->is_array) inherited = BuiltinProtoLookup(interp, "Array", key);
        if (inherited.type == VType::Undefined && (objc.value.obj->is_function || objc.value.obj->is_class)) inherited = BuiltinProtoLookup(interp, "Function", key);
        if (inherited.type == VType::Undefined) inherited = BuiltinProtoLookup(interp, "Object", key);
        if (inherited.type != VType::Undefined) {
            if (inherited.type == VType::Object && inherited.obj && inherited.obj->is_function) { interp.member_receiver = objc.value; interp.member_receiver_fn = inherited.obj.get(); }
            return Completion::Norm(inherited);
        }
    }
    if (key == "toString") {
        const Value self = objc.value;
        return Completion::Norm(MakeNativeFn([self](const std::vector<Value> &, bool &, std::string &) { return Value::Str(ToDisplayString(self)); }));
    }
    return Completion::Norm(ordinary);
}

Completion EvalExpr(Interpreter &interp, const Node &n, EnvPtr &env) {
    Completion guard;
    if (interp.StepGuard(guard)) return guard;

    switch (n.kind) {
        case NodeKind::NumberLit:
            return Completion::Norm(Value::Num(n.num));
        case NodeKind::StringLit:
            return Completion::Norm(Value::Str(n.str));
        case NodeKind::BoolLit:
            return Completion::Norm(Value::Bool(n.boolean));
        case NodeKind::NullLit:
            return Completion::Norm(Value::MakeNull());
        case NodeKind::UndefinedLit:
            return Completion::Norm(Value::Undef());
        case NodeKind::Await: {
            Completion awaited = EvalExpr(interp, *n.a, env);
            if (awaited.IsAbrupt()) return awaited;
#if defined(MEP_JS_COROUTINES)
            if (Coroutine *co = g_current_coroutine; co && co->is_async) {
                // Always a real suspension (even for a settled promise or a
                // plain value), which is what orders `await` after the
                // synchronous code and earlier microtasks.
                ObjectPtr promise;
                if (awaited.value.type == VType::Object && awaited.value.obj && awaited.value.obj->is_promise) promise = awaited.value.obj;
                else {
                    Completion thenable = awaited.value.type == VType::Object && awaited.value.obj ? MemberGet(interp, awaited.value, "then") : Completion::Norm();
                    promise = std::make_shared<ObjectData>();
                    promise->is_promise = true;
                    if (!thenable.IsAbrupt() && thenable.value.type == VType::Object && thenable.value.obj && thenable.value.obj->is_function) {
                        ObjectPtr adopted = promise;
                        std::vector<Value> then_args{
                            MakeNativeFn([&interp, adopted](const std::vector<Value> &values, bool &, std::string &) { SettlePromise(interp, adopted, 1, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }),
                            MakeNativeFn([&interp, adopted](const std::vector<Value> &values, bool &, std::string &) { SettlePromise(interp, adopted, 2, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); })};
                        Completion subscribed = CallFunction(interp, thenable.value.obj, then_args, &awaited.value);
                        if (subscribed.IsAbrupt()) return subscribed;
                    } else SettlePromise(interp, promise, 1, awaited.value);
                }
                Completion then = MemberGet(interp, Value::Obj(promise), "then");
                if (then.IsAbrupt()) return then;
                auto resume = [&interp, co](int mode) {
                    return MakeNativeFn([&interp, co, mode](const std::vector<Value> &values, bool &, std::string &) {
                        co->sent = values.empty() ? Value::Undef() : values[0];
                        co->sent_mode = mode;
                        ResumeCoroutine(interp, *co);
                        if (co->finished && co->on_finish) { auto done = std::move(co->on_finish); co->on_finish = nullptr; done(); }
                        return Value::Undef();
                    });
                };
                std::vector<Value> reactions{resume(0), resume(1)};
                Completion subscribed = CallFunction(interp, then.value.obj, reactions);
                if (subscribed.IsAbrupt()) return subscribed;
                SuspendCoroutine(*co);
                return co->sent_mode == 1 ? Completion::Thr(co->sent) : Completion::Norm(co->sent);
            }
#endif
            if (awaited.value.type != VType::Object || !awaited.value.obj || !awaited.value.obj->is_promise)
                return awaited;
            if (awaited.value.obj->promise_state == 0) {
                Completion checkpoint = DrainMicrotasks(interp);
                if (checkpoint.type == CompletionType::Throw) return checkpoint;
            }
            if (awaited.value.obj->promise_state == 1) return Completion::Norm(awaited.value.obj->promise_value);
            if (awaited.value.obj->promise_state == 2) return Completion::Thr(awaited.value.obj->promise_value);
            return Completion::Thr("await on a pending promise requires the persistent event loop");
        }
        case NodeKind::TemplateLit: {
            std::string out;
            size_t expr_i = 0;
            for (size_t i = 0; i < n.is_expr_part.size(); i++) {
                if (!n.is_expr_part[i]) {
                    out += n.template_texts[i];
                } else {
                    Completion c = EvalExpr(interp, *n.template_exprs[expr_i++], env);
                    if (c.IsAbrupt()) return c;
                    out += ToDisplayString(c.value);
                }
            }
            return Completion::Norm(Value::Str(out));
        }
        case NodeKind::ArrayLit: {
            auto obj = std::make_shared<ObjectData>();
            obj->is_array = true;
            size_t out_index = 0;
            for (size_t i = 0; i < n.elements.size(); i++) {
                Completion c = EvalExpr(interp, *n.elements[i], env);
                if (c.IsAbrupt()) return c;
                const bool spread = i < n.element_spread.size() && n.element_spread[i];
                if (spread) {
                    std::vector<Value> spread_values;
                    std::string spread_error;
                    if (!IterateValues(interp, c.value, spread_values, spread_error)) return Completion::Thr(spread_error);
                    for (Value &item : spread_values) obj->props[std::to_string(out_index++)] = std::move(item);
                } else {
                    obj->props[std::to_string(out_index++)] = c.value;
                }
            }
            obj->props["length"] = Value::Num(static_cast<double>(out_index));
            return Completion::Norm(Value::Obj(obj));
        }
        case NodeKind::ObjectLit: {
            auto obj = std::make_shared<ObjectData>();
            if (Value *object_ctor = interp.global->Find("Object"); object_ctor && object_ctor->type == VType::Object && object_ctor->obj) {
                Value prototype = GetProp(object_ctor->obj, "prototype");
                if (prototype.type == VType::Object) obj->prototype = prototype.obj;
            }
            for (size_t i = 0; i < n.obj_props.size(); ++i) {
                const auto &kv = n.obj_props[i];
                const bool spread = i < n.obj_prop_spread.size() && n.obj_prop_spread[i];
                if (spread) {
                    Completion source = EvalExpr(interp, *kv.second, env);
                    if (source.IsAbrupt()) return source;
                    if (source.value.type != VType::Object || !source.value.obj)
                        return Completion::Thr("object spread requires an object");
                    for (const std::string &spread_key : EnumerableKeys(source.value.obj)) obj->props[spread_key] = GetProp(source.value.obj, spread_key);
                    continue;
                }
                std::string key = kv.first;
                if (i < n.obj_prop_key_exprs.size() && n.obj_prop_key_exprs[i]) {
                    Completion key_completion = EvalExpr(interp, *n.obj_prop_key_exprs[i], env);
                    if (key_completion.IsAbrupt()) return key_completion;
                    key = PropertyKey(key_completion.value);
                }
                Completion c = EvalExpr(interp, *kv.second, env);
                if (c.IsAbrupt()) return c;
                const int accessor = i < n.obj_prop_accessor.size() ? n.obj_prop_accessor[i] : 0;
                if (accessor != 0 && c.value.type == VType::Object && c.value.obj) {
                    (accessor == 1 ? obj->getters : obj->setters)[key] = c.value.obj;
                    continue;
                }
                obj->props[key] = c.value;
            }
            return Completion::Norm(Value::Obj(obj));
        }
        case NodeKind::Ident: {
            const Value *slot = env->Find(n.name);
            if (slot) return Completion::Norm(*slot);
            if (interp.window_object && (HasProperty(interp.window_object, n.name) || GetProp(interp.window_object, n.name).type != VType::Undefined)) {
                return Completion::Norm(GetProp(interp.window_object, n.name));
            }
            return Completion::Thr("'" + n.name + "' is not defined" + SourceExcerpt(n));
        }
        case NodeKind::Yield: {
            Value value = Value::Undef();
            if (n.a) {
                Completion yielded = EvalExpr(interp, *n.a, env);
                if (yielded.IsAbrupt()) return yielded;
                value = yielded.value;
            }
#if defined(MEP_JS_COROUTINES)
            Coroutine *co = g_current_coroutine;
            if (!co || co->is_async) return Completion::Thr("yield outside a generator");
            auto hand_out = [&](Value item) -> Completion {
                co->yielded = std::move(item);
                SuspendCoroutine(*co);
                if (co->sent_mode == 1) return Completion::Thr(co->sent);
                if (co->sent_mode == 2) return Completion::Ret(co->sent);  // runs enclosing finally blocks on the way out
                return Completion::Norm(co->sent);
            };
            if (n.boolean) {  // yield* iterable
                std::vector<Value> delegated;
                std::string iterate_error;
                if (!IterateValues(interp, value, delegated, iterate_error)) return Completion::Thr(iterate_error);
                for (Value &item : delegated) {
                    Completion resumed = hand_out(std::move(item));
                    if (resumed.IsAbrupt()) return resumed;
                }
                return Completion::Norm(Value::Undef());
            }
            return hand_out(std::move(value));
#else
            if (!interp.yield_values) return Completion::Thr("yield outside generator");
            interp.yield_values->push_back(std::move(value));
            return Completion::Norm(Value::Undef());
#endif
        }
        case NodeKind::ImportDecl:
            return Completion::Norm();
        case NodeKind::ExportDefault: {
            Completion value = EvalExpr(interp, *n.a, env);
            if (value.IsAbrupt()) return value;
            env->FunctionScope()->Define("*default*", value.value);
            return Completion::Norm();
        }
        case NodeKind::ImportMeta: {
            auto meta = std::make_shared<ObjectData>();
            const Value *url = env->Find("*module-url*");
            meta->props["url"] = url ? *url : Value::Str(interp.document_url);
            return Completion::Norm(Value::Obj(meta));
        }
        case NodeKind::DynamicImport: {
            Completion specifier = EvalExpr(interp, *n.a, env);
            if (specifier.IsAbrupt()) return specifier;
            const Value *base = env->Find("*module-url*");
            const std::string url = urlutil::ResolveUrl(base ? ToDisplayString(*base) : interp.document_url, ToDisplayString(specifier.value));
            auto promise = std::make_shared<ObjectData>();
            promise->is_promise = true;
            Completion loaded = interp.load_module ? interp.load_module(url) : Completion::Thr("import() is unavailable here");
            SettlePromise(interp, promise, loaded.type == CompletionType::Throw ? 2 : 1, loaded.type == CompletionType::Throw ? interp.WrapThrown(loaded.value) : loaded.value);
            return Completion::Norm(Value::Obj(promise));
        }
        case NodeKind::Sequence: {
            Completion last = Completion::Norm();
            for (const auto &part : n.body) {
                last = EvalExpr(interp, *part, env);
                if (last.IsAbrupt()) return last;
            }
            return last;
        }
        case NodeKind::RegexLit: {
            Value *ctor = interp.global->Find("RegExp");
            if (!ctor || ctor->type != VType::Object || !ctor->obj) return Completion::Thr("RegExp is unavailable");
            std::vector<Value> ctor_args{Value::Str(n.str), Value::Str(n.op)};
            return CallFunction(interp, ctor->obj, ctor_args);
        }
        case NodeKind::Unary: {
            if (n.op == "typeof" && n.a->kind == NodeKind::Ident && !env->Find(n.a->name) &&
                !(interp.window_object && (HasProperty(interp.window_object, n.a->name) || GetProp(interp.window_object, n.a->name).type != VType::Undefined))) {
                return Completion::Norm(Value::Str("undefined"));  // never a ReferenceError
            }
            if (n.op == "delete") {
                if (n.a->kind != NodeKind::Member) return Completion::Norm(Value::Bool(true));
                Completion target = EvalExpr(interp, *n.a->a, env);
                if (target.IsAbrupt()) return target;
                if (target.value.type != VType::Object || !target.value.obj) return Completion::Norm(Value::Bool(true));
                std::string key = n.a->prop_name;
                if (n.a->computed) {
                    Completion keyc = EvalExpr(interp, *n.a->b, env);
                    if (keyc.IsAbrupt()) return keyc;
                    key = PropertyKey(keyc.value);
                }
                if (target.value.obj->frozen) return Completion::Norm(Value::Bool(false));
                target.value.obj->props.erase(key);
                target.value.obj->getters.erase(key);
                target.value.obj->setters.erase(key);
                return Completion::Norm(Value::Bool(true));
            }
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            if (n.op == "void") return Completion::Norm(Value::Undef());
            if (n.op == "~") return Completion::Norm(Value::Num(~ToInt32(ToNumber(c.value))));
            if (n.op == "-") return Completion::Norm(Value::Num(-ToNumber(c.value)));
            if (n.op == "+") return Completion::Norm(Value::Num(ToNumber(c.value)));
            if (n.op == "!") return Completion::Norm(Value::Bool(!c.value.Truthy()));
            if (n.op == "typeof") {
                switch (c.value.type) {
                    case VType::Undefined:
                        return Completion::Norm(Value::Str("undefined"));
                    case VType::Null:
                        return Completion::Norm(Value::Str("object"));
                    case VType::Number:
                        return Completion::Norm(Value::Str("number"));
                    case VType::String:
                        return Completion::Norm(Value::Str("string"));
                    case VType::Boolean:
                        return Completion::Norm(Value::Str("boolean"));
                    case VType::Object:
                        if (c.value.obj && c.value.obj->is_symbol) return Completion::Norm(Value::Str("symbol"));
                        return Completion::Norm(Value::Str(c.value.obj && (c.value.obj->is_function || c.value.obj->is_class) ? "function" : "object"));
                }
            }
            return Completion::Thr("unsupported unary operator");
        }
        case NodeKind::Update: {
            Completion cur = EvalExpr(interp, *n.a, env);
            if (cur.IsAbrupt()) return cur;
            double old_val = ToNumber(cur.value);
            double new_val = n.op == "++" ? old_val + 1 : old_val - 1;
            Completion out;
            if (!AssignTo(interp, *n.a, Value::Num(new_val), env, out)) return out;
            // Prefix yields the updated value (n.boolean==true, see ParseUnary);
            // postfix yields the pre-update value -- the one real behavioral
            // difference between `++i` and `i++` this engine bothers to model.
            return Completion::Norm(Value::Num(n.boolean ? new_val : old_val));
        }
        case NodeKind::Logical: {
            Completion l = EvalExpr(interp, *n.a, env);
            if (l.IsAbrupt()) return l;
            if (n.op == "&&") return l.value.Truthy() ? EvalExpr(interp, *n.b, env) : l;
            if (n.op == "??") {
                return (l.value.type == VType::Null || l.value.type == VType::Undefined) ? EvalExpr(interp, *n.b, env) : l;
            }
            return l.value.Truthy() ? l : EvalExpr(interp, *n.b, env);
        }
        case NodeKind::Binary: {
            Completion l = EvalExpr(interp, *n.a, env);
            if (l.IsAbrupt()) return l;
            Completion r = EvalExpr(interp, *n.b, env);
            if (r.IsAbrupt()) return r;
            return ApplyBinary(n.op, l.value, r.value);
        }
        case NodeKind::Assign: {
            Completion rhs;
            if (n.op == "=") {
                rhs = EvalExpr(interp, *n.b, env);
            } else {
                Completion cur = EvalExpr(interp, *n.a, env);
                if (cur.IsAbrupt()) return cur;
                // Logical assignment short-circuits: the right side only
                // runs (and the target is only written) when it decides the result.
                if (n.op == "&&=" && !cur.value.Truthy()) return Completion::Norm(cur.value);
                if (n.op == "||=" && cur.value.Truthy()) return Completion::Norm(cur.value);
                if (n.op == "?\?=" && cur.value.type != VType::Null && cur.value.type != VType::Undefined) return Completion::Norm(cur.value);
                Completion r = EvalExpr(interp, *n.b, env);
                if (r.IsAbrupt()) return r;
                if (n.op == "&&=" || n.op == "||=" || n.op == "?\?=") rhs = Completion::Norm(r.value);
                else rhs = ApplyBinary(n.op.substr(0, n.op.size() - 1), cur.value, r.value);
            }
            if (rhs.IsAbrupt()) return rhs;
            if (n.op == "=" && (n.a->kind == NodeKind::ArrayLit || n.a->kind == NodeKind::ObjectLit)) {
                Completion assigned = AssignPattern(interp, *n.a, rhs.value, env);
                if (assigned.IsAbrupt()) return assigned;
                return Completion::Norm(rhs.value);
            }
            Completion out;
            if (!AssignTo(interp, *n.a, rhs.value, env, out)) return out;
            return Completion::Norm(rhs.value);
        }
        case NodeKind::Member: {
            Completion objc = EvalExpr(interp, *n.a, env);
            if (objc.IsAbrupt()) return objc;
            if (objc.value.type == VType::Null || objc.value.type == VType::Undefined) {
                if (n.optional) return Completion::Norm(Value::Undef());
                return Completion::Thr("cannot read property '" + (n.computed ? std::string("[...]") : n.prop_name) + "' of " + ToDisplayString(objc.value) + " (" + DescribeExpr(*n.a) + ")" + SourceExcerpt(n));
            }
            std::string key = n.prop_name;
            if (n.computed) {
                Completion keyc = EvalExpr(interp, *n.b, env);
                if (keyc.IsAbrupt()) return keyc;
                key = PropertyKey(keyc.value);
            }
            return MemberGet(interp, objc.value, key);
        }
        case NodeKind::Call: {
            Completion calleec = EvalExpr(interp, *n.a, env);
            if (calleec.IsAbrupt()) return calleec;
            // A method call: the Member evaluation just above left its base here.
            const bool has_receiver = n.a->kind == NodeKind::Member && calleec.value.type == VType::Object && calleec.value.obj &&
                                      interp.member_receiver_fn == calleec.value.obj.get();
            Value receiver = has_receiver ? interp.member_receiver : Value::Undef();
            interp.member_receiver_fn = nullptr;
            if (calleec.value.type != VType::Object || !calleec.value.obj || !calleec.value.obj->is_function) {
                const bool chained_optional_member = n.a && n.a->kind == NodeKind::Member && n.a->optional;
                if ((n.optional || chained_optional_member) &&
                    (calleec.value.type == VType::Null || calleec.value.type == VType::Undefined)) {
                    return Completion::Norm(Value::Undef());
                }
                return Completion::Thr(DescribeExpr(*n.a) + " is not a function" + SourceExcerpt(n));
            }
            std::vector<Value> args;
            for (size_t i = 0; i < n.args.size(); ++i) {
                const auto &a = n.args[i];
                Completion c = EvalExpr(interp, *a, env);
                if (c.IsAbrupt()) return c;
                const bool spread = i < n.arg_spread.size() && n.arg_spread[i];
                if (spread) {
                    std::string spread_error;
                    if (!IterateValues(interp, c.value, args, spread_error)) return Completion::Thr(spread_error);
                } else {
                    args.push_back(c.value);
                }
            }
            if (has_receiver) return CallFunction(interp, calleec.value.obj, args, &receiver);
            return CallFunction(interp, calleec.value.obj, args);
        }
        case NodeKind::TaggedCall: {
            Completion callee = EvalExpr(interp, *n.a, env);
            if (callee.IsAbrupt()) return callee;
            if (callee.value.type != VType::Object || !callee.value.obj || !callee.value.obj->is_function) return Completion::Thr("tag is not a function");
            const Node &template_node = *n.b;
            auto strings = std::make_shared<ObjectData>(); strings->is_array = true;
            size_t string_index = 0;
            for (size_t i = 0; i < template_node.is_expr_part.size(); ++i) if (!template_node.is_expr_part[i]) strings->props[std::to_string(string_index++)] = Value::Str(template_node.template_texts[i]);
            strings->props["length"] = Value::Num(static_cast<double>(string_index));
            std::vector<Value> args{Value::Obj(strings)};
            for (const auto &expression : template_node.template_exprs) {
                Completion value = EvalExpr(interp, *expression, env);
                if (value.IsAbrupt()) return value;
                args.push_back(value.value);
            }
            return CallFunction(interp, callee.value.obj, args);
        }
        case NodeKind::New: {
            Completion class_value = EvalExpr(interp, *n.a, env);
            if (class_value.IsAbrupt()) return class_value;
            if (class_value.value.type != VType::Object || !class_value.value.obj)
                return Completion::Thr("value is not a class constructor");
            std::vector<Value> args;
            for (size_t i = 0; i < n.args.size(); ++i) {
                    Completion argument = EvalExpr(interp, *n.args[i], env);
                    if (argument.IsAbrupt()) return argument;
                    args.push_back(argument.value);
            }
            if (!class_value.value.obj->is_class) {
                if (!class_value.value.obj->is_function) return Completion::Thr("value is not a constructor");
                const ObjectPtr &ctor = class_value.value.obj;
                // Natives (Map, Date, Error, ...) build their own result.
                if (ctor->native || !ctor->fn_node || ctor->is_arrow_function) return CallFunction(interp, ctor, args);
                auto created = std::make_shared<ObjectData>();
                Value proto = GetProp(ctor, "prototype");
                if (proto.type == VType::Object && proto.obj) created->prototype = proto.obj;
                Value receiver = Value::Obj(created);
                Completion constructed = CallFunction(interp, ctor, args, &receiver);
                if (constructed.IsAbrupt()) return constructed;
                // An object returned from the constructor replaces the new instance.
                if (constructed.value.type == VType::Object && constructed.value.obj) return constructed;
                return Completion::Norm(receiver);
            }
            auto instance = std::make_shared<ObjectData>();
            instance->prototype = class_value.value.obj->prototype;
            EnvPtr field_scope = std::make_shared<Environment>();
            field_scope->parent = env;
            field_scope->Define("this", Value::Obj(instance));
            for (size_t i = 0; i < class_value.value.obj->private_field_names.size(); ++i) {
                Value initial = Value::Undef();
                if (class_value.value.obj->class_node && i < class_value.value.obj->class_node->class_private_initializers.size() && class_value.value.obj->class_node->class_private_initializers[i]) {
                    Completion initialized = EvalExpr(interp, *class_value.value.obj->class_node->class_private_initializers[i], field_scope);
                    if (initialized.IsAbrupt()) return initialized;
                    initial = initialized.value;
                }
                SetProp(instance, class_value.value.obj->private_field_names[i], initial);
            }
            // Public instance fields, base class first.
            {
                std::vector<const Node *> chain;
                for (ObjectPtr k = class_value.value.obj; k && k->class_node; k = k->super_class) chain.push_back(k->class_node);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const auto &field : (*it)->class_instance_fields) {
                        Value initial = Value::Undef();
                        if (field.second) {
                            Completion initialized = EvalExpr(interp, *field.second, field_scope);
                            if (initialized.IsAbrupt()) return initialized;
                            initial = initialized.value;
                        }
                        instance->props[field.first] = initial;
                    }
                }
            }
            Value instance_value = Value::Obj(instance);
            Value constructor = GetProp(class_value.value.obj, "constructor");
            if (constructor.type == VType::Object && constructor.obj && constructor.obj->is_function) {
                Completion constructed = CallFunction(interp, constructor.obj, args, &instance_value);
                if (constructed.IsAbrupt()) return constructed;
                // An inherited native constructor (class X extends Error {}) returns its own object.
                if (constructor.obj->native && constructed.value.type == VType::Object && constructed.value.obj) {
                    for (const auto &entry : constructed.value.obj->props) instance->props[entry.first] = entry.second;
                }
            }
            return Completion::Norm(instance_value);
        }
        case NodeKind::Conditional: {
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            return c.value.Truthy() ? EvalExpr(interp, *n.b, env) : EvalExpr(interp, *n.c, env);
        }
        case NodeKind::FunctionExpr: {
            auto obj = std::make_shared<ObjectData>();
            obj->is_function = true;
            obj->is_generator_function = n.is_generator;
            obj->is_async_function = n.is_async;
            obj->is_arrow_function = n.is_arrow;
            obj->fn_node = &n;
            obj->closure = env;
            return Completion::Norm(Value::Obj(obj));
        }
        default:
            return Completion::Thr("expression not supported in this context");
    }
}

// Bind a declaration pattern recursively.  Defaults are evaluated lazily in
// the surrounding declaration scope, after earlier names in the same pattern
// have been established, which covers the useful JavaScript idiom
// `var [a, b = a] = values`.
Completion BindPattern(Interpreter &interp, const BindingPattern &pattern, Value value, EnvPtr &env) {
    if (value.type == VType::Undefined && pattern.default_value) {
        Completion fallback = EvalExpr(interp, *pattern.default_value, env);
        if (fallback.IsAbrupt()) return fallback;
        value = fallback.value;
    }
    switch (pattern.kind) {
        case BindingPattern::Kind::Ident:
            env->Define(pattern.name, value);
            return Completion::Norm();
        case BindingPattern::Kind::Array: {
            if (value.type != VType::Object || !value.obj || !value.obj->is_array) {
                std::vector<Value> drained;
                std::string iterate_error;
                if (!IterateValues(interp, value, drained, iterate_error)) return Completion::Thr(iterate_error);
                value = MakeArray(drained);
            }
            for (size_t i = 0; i < pattern.elements.size(); ++i) {
                if (!pattern.elements[i]) continue;
                Completion bound = BindPattern(interp, *pattern.elements[i], GetProp(value.obj, std::to_string(i)), env);
                if (bound.IsAbrupt()) return bound;
            }
            if (pattern.rest) {
                auto rest = std::make_shared<ObjectData>();
                rest->is_array = true;
                long out_index = 0;
                for (long i = static_cast<long>(pattern.elements.size()); i < ArrayLength(value.obj); ++i) rest->props[std::to_string(out_index++)] = GetProp(value.obj, std::to_string(i));
                rest->props["length"] = Value::Num(static_cast<double>(out_index));
                return BindPattern(interp, *pattern.rest, Value::Obj(rest), env);
            }
            return Completion::Norm();
        }
        case BindingPattern::Kind::Object:
            if (value.type != VType::Object || !value.obj)
                return Completion::Thr("object destructuring requires an object");
            for (const auto &property : pattern.properties) {
                Completion bound = BindPattern(interp, *property.second, GetProp(value.obj, property.first), env);
                if (bound.IsAbrupt()) return bound;
            }
            if (pattern.rest) {
                auto rest = std::make_shared<ObjectData>();
                for (const auto &entry : value.obj->props) {
                    bool taken = false;
                    for (const auto &property : pattern.properties) taken = taken || property.first == entry.first;
                    if (!taken) rest->props[entry.first] = entry.second;
                }
                return BindPattern(interp, *pattern.rest, Value::Obj(rest), env);
            }
            return Completion::Norm();
    }
    return Completion::Thr("invalid binding pattern");
}

/**
 * @brief Executes a statement AST node, dispatching on the node's kind (block, expression statement, var declaration, function declaration, if, while, for, return, break, continue), falling back to expression evaluation for any other node kind.
 * @param interp The interpreter, providing step-counting/call-depth state shared across the whole run.
 * @param n The statement node to execute.
 * @param env The scope to execute it in.
 * @return Normal after a non-control-flow statement, or the abrupt completion produced by (or propagated through) it: Return, Break, Continue, or Throw.
 */
Completion ExecStmt(Interpreter &interp, const Node &n, EnvPtr &env) {
    switch (n.kind) {
        case NodeKind::Block: {
            EnvPtr inner = std::make_shared<Environment>();
            inner->parent = env;
            return ExecBlockBody(interp, n.body, inner);
        }
        case NodeKind::ExprStmt:
            return EvalExpr(interp, *n.a, env);
        case NodeKind::VarDecl: {
            // `var` binds in the enclosing function (or program) scope no
            // matter how deep the block; let/const bind right here.
            EnvPtr var_scope;
            if (n.boolean) {
                Environment *function_scope = env->FunctionScope();
                for (EnvPtr e = env; e; e = e->parent) if (e.get() == function_scope) { var_scope = e; break; }
            }
            EnvPtr &target = var_scope ? var_scope : env;
            auto run_simple = [&](const std::pair<std::string, NodePtr> &d) -> Completion {
                if (!d.second) {
                    // `var x;` re-declares without resetting; `let x;` starts undefined.
                    if (!n.boolean || !target->vars.count(d.first)) target->Define(d.first, Value::Undef());
                    return Completion::Norm();
                }
                Completion c = EvalExpr(interp, *d.second, env);
                if (c.IsAbrupt()) return c;
                target->Define(d.first, c.value);
                return Completion::Norm();
            };
            auto run_pattern = [&](const std::pair<std::unique_ptr<BindingPattern>, NodePtr> &d) -> Completion {
                Value v = Value::Undef();
                if (d.second) {
                    Completion c = EvalExpr(interp, *d.second, env);
                    if (c.IsAbrupt()) return c;
                    v = c.value;
                }
                return BindPattern(interp, *d.first, v, target);
            };
            // Declarators run in source order: `let [a] = f(), b = a + 1`.
            size_t simple_index = 0, pattern_index = 0;
            for (size_t k = 0; k < n.declarator_order.size(); ++k) {
                Completion done = n.declarator_order[k] ? run_pattern(n.pattern_declarators[pattern_index++]) : run_simple(n.declarators[simple_index++]);
                if (done.IsAbrupt()) return done;
            }
            // Declarations built elsewhere (for-loop heads) carry no order list.
            for (; simple_index < n.declarators.size(); ++simple_index) { Completion done = run_simple(n.declarators[simple_index]); if (done.IsAbrupt()) return done; }
            for (; pattern_index < n.pattern_declarators.size(); ++pattern_index) { Completion done = run_pattern(n.pattern_declarators[pattern_index]); if (done.IsAbrupt()) return done; }
            return Completion::Norm();
        }
        case NodeKind::FunctionDecl:
            return Completion::Norm();  // already bound by ExecBlockBody's hoisting pass
        case NodeKind::Class: {
            ObjectPtr base_class;
            if (n.a) {
                Completion base = EvalExpr(interp, *n.a, env);
                if (base.IsAbrupt()) return base;
                if (base.value.type != VType::Object || !base.value.obj || !(base.value.obj->is_class || base.value.obj->is_function))
                    return Completion::Thr("class extends requires a constructor");
                base_class = base.value.obj;
            }
            auto klass = std::make_shared<ObjectData>();
            klass->is_class = true;
            klass->class_node = &n;
            klass->super_class = base_class;
            klass->private_field_names = n.class_private_fields;
            klass->prototype = std::make_shared<ObjectData>();
            if (base_class) {
                if (base_class->is_class) klass->prototype->prototype = base_class->prototype;
                else if (Value base_proto = GetProp(base_class, "prototype"); base_proto.type == VType::Object) klass->prototype->prototype = base_proto.obj;
            }
            for (const auto &field : n.class_static_fields) {
                Value initial = Value::Undef();
                if (field.second) {
                    Completion initialized = EvalExpr(interp, *field.second, env);
                    if (initialized.IsAbrupt()) return initialized;
                    initial = initialized.value;
                }
                SetProp(klass, field.first, initial);
            }
            for (size_t i = 0; i < n.body.size(); ++i) {
                auto method = std::make_shared<ObjectData>();
                method->is_function = true;
                method->is_generator_function = n.body[i]->is_generator;
                method->fn_node = n.body[i].get();
                method->closure = env;
                method->super_class = base_class;
                const bool is_static = i < n.class_method_static.size() && n.class_method_static[i];
                ObjectPtr holder = is_static ? klass : klass->prototype;
                const int accessor = i < n.class_method_accessor.size() ? n.class_method_accessor[i] : 0;
                if (accessor == 1) holder->getters[n.class_method_names[i]] = method;
                else if (accessor == 2) holder->setters[n.class_method_names[i]] = method;
                else SetProp(holder, n.class_method_names[i], Value::Obj(method));
            }
            env->Define(n.name, Value::Obj(klass));
            return Completion::Norm();
        }
        case NodeKind::If: {
            Completion c = EvalExpr(interp, *n.cond, env);
            if (c.IsAbrupt()) return c;
            if (c.value.Truthy()) return ExecStmt(interp, *n.then_branch, env);
            if (n.else_branch) return ExecStmt(interp, *n.else_branch, env);
            return Completion::Norm();
        }
        case NodeKind::While: {
            for (;;) {
                Completion guard;
                if (interp.StepGuard(guard)) return guard;
                Completion c = EvalExpr(interp, *n.cond, env);
                if (c.IsAbrupt()) return c;
                if (!c.value.Truthy()) break;
                Completion body = ExecStmt(interp, *n.then_branch, env);
                if (body.type == CompletionType::Break) {
                    if (body.label.empty() || body.label == n.name) break;
                    return body;
                }
                if (body.type == CompletionType::Continue && !body.label.empty() && body.label != n.name) return body;
                if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
            }
            return Completion::Norm();
        }
        case NodeKind::DoWhile: {
            for (;;) {
                Completion guard;
                if (interp.StepGuard(guard)) return guard;
                Completion body = ExecStmt(interp, *n.then_branch, env);
                if (body.type == CompletionType::Break) {
                    if (body.label.empty() || body.label == n.name) break;
                    return body;
                }
                if (body.type == CompletionType::Continue && !body.label.empty() && body.label != n.name) return body;
                if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
                Completion c = EvalExpr(interp, *n.cond, env);
                if (c.IsAbrupt()) return c;
                if (!c.value.Truthy()) break;
            }
            return Completion::Norm();
        }
        case NodeKind::For: {
            EnvPtr loop_env = std::make_shared<Environment>();
            loop_env->parent = env;
            if (n.op == "of" || n.op == "in") {
                Completion source = EvalExpr(interp, *n.a, loop_env);
                if (source.IsAbrupt()) return source;
                // One pass of the body for one value. `stop` is set when the
                // loop is over (break, or an abrupt completion to propagate).
                auto run_body = [&](Value value, bool &stop) -> Completion {
                    Completion guard;
                    if (interp.StepGuard(guard)) { stop = true; return guard; }
                    EnvPtr iteration_env = std::make_shared<Environment>();
                    iteration_env->parent = loop_env;
                    if (!n.pattern_declarators.empty()) {
                        Completion bound = BindPattern(interp, *n.pattern_declarators[0].first, std::move(value), iteration_env);
                        if (bound.IsAbrupt()) { stop = true; return bound; }
                    } else if (n.boolean) {
                        iteration_env->Define(n.name, std::move(value));
                    } else {
                        Value *slot = loop_env->Find(n.name);
                        if (slot) *slot = std::move(value);
                        else interp.global->Define(n.name, std::move(value));
                    }
                    Completion body = ExecStmt(interp, *n.then_branch, iteration_env);
                    if (body.type == CompletionType::Break) {
                        stop = true;
                        return (body.label.empty() || body.label == n.name) ? Completion::Norm() : body;
                    }
                    if (body.type == CompletionType::Continue && !body.label.empty() && body.label != n.name) { stop = true; return body; }
                    if (body.type == CompletionType::Return || body.type == CompletionType::Throw) { stop = true; return body; }
                    return Completion::Norm();
                };
                const bool builtin_iterable = source.value.type == VType::String ||
                                              (source.value.type == VType::Object && source.value.obj &&
                                               (source.value.obj->is_array || source.value.obj->is_map || source.value.obj->is_set));
                if (n.op == "of" && !builtin_iterable) {
                    // The iterator protocol, one step per pass: a generator
                    // may be infinite, and leaving early must call return().
                    if (source.value.type != VType::Object || !source.value.obj) return Completion::Thr(ToDisplayString(source.value) + " is not iterable");
                    Value iterator = source.value;
                    Completion factory = MemberGet(interp, source.value, "@@iterator");
                    if (factory.IsAbrupt()) return factory;
                    if (factory.value.type == VType::Object && factory.value.obj && factory.value.obj->is_function) {
                        std::vector<Value> no_args;
                        Completion made = CallFunction(interp, factory.value.obj, no_args, &source.value);
                        if (made.IsAbrupt()) return made;
                        iterator = made.value;
                    }
                    if (iterator.type != VType::Object || !iterator.obj) return Completion::Thr("object is not iterable");
                    Value next = GetProp(iterator.obj, "next");
                    if (next.type != VType::Object || !next.obj || !next.obj->is_function) return Completion::Thr("object is not iterable");
                    for (;;) {
                        std::vector<Value> no_args;
                        Completion step = CallFunction(interp, next.obj, no_args, &iterator);
                        if (step.IsAbrupt()) return step;
                        if (step.value.type != VType::Object || !step.value.obj) return Completion::Thr("iterator result is not an object");
                        if (GetProp(step.value.obj, "done").Truthy()) return Completion::Norm();
                        bool stop = false;
                        Completion pass = run_body(GetProp(step.value.obj, "value"), stop);
                        if (stop) {
                            Value closer = GetProp(iterator.obj, "return");
                            if (closer.type == VType::Object && closer.obj && closer.obj->is_function) { std::vector<Value> close_args; (void)CallFunction(interp, closer.obj, close_args, &iterator); }
                            return pass;
                        }
                    }
                }
                std::vector<Value> values;
                if (n.op == "of") {
                    std::string iterate_error;
                    if (!IterateValues(interp, source.value, values, iterate_error)) return Completion::Thr(iterate_error);
                } else if (source.value.type == VType::Object && source.value.obj) {
                    // Own keys, then each prototype's, never the same name twice.
                    std::unordered_set<std::string> seen;
                    for (ObjectPtr cur = source.value.obj; cur; cur = cur->prototype) {
                        for (std::string &key : EnumerableKeys(cur)) if (seen.insert(key).second) values.push_back(Value::Str(std::move(key)));
                    }
                }
                for (Value &value : values) {
                    bool stop = false;
                    Completion pass = run_body(std::move(value), stop);
                    if (stop) return pass;
                }
                return Completion::Norm();
            }
            if (n.init) {
                Completion c = ExecStmt(interp, *n.init, loop_env);
                if (c.IsAbrupt()) return c;
            }
            for (;;) {
                Completion guard;
                if (interp.StepGuard(guard)) return guard;
                if (n.cond) {
                    Completion c = EvalExpr(interp, *n.cond, loop_env);
                    if (c.IsAbrupt()) return c;
                    if (!c.value.Truthy()) break;
                }
                Completion body = ExecStmt(interp, *n.then_branch, loop_env);
                if (body.type == CompletionType::Break) {
                    if (body.label.empty() || body.label == n.name) break;
                    return body;
                }
                if (body.type == CompletionType::Continue && !body.label.empty() && body.label != n.name) return body;
                if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
                if (!loop_env->vars.empty()) {
                    EnvPtr next_env = std::make_shared<Environment>();
                    next_env->parent = env;
                    next_env->vars = loop_env->vars;
                    loop_env = next_env;
                }
                if (n.update) {
                    Completion c = EvalExpr(interp, *n.update, loop_env);
                    if (c.IsAbrupt()) return c;
                }
            }
            return Completion::Norm();
        }
        case NodeKind::Switch: {
            Completion discriminant = EvalExpr(interp, *n.cond, env);
            if (discriminant.IsAbrupt()) return discriminant;
            size_t default_index = n.switch_cases.size();
            size_t start = n.switch_cases.size();
            for (size_t i = 0; i < n.switch_cases.size(); ++i) {
                const auto &entry = n.switch_cases[i];
                if (!entry.first) { default_index = i; continue; }
                Completion test = EvalExpr(interp, *entry.first, env);
                if (test.IsAbrupt()) return test;
                if (StrictEquals(discriminant.value, test.value)) { start = i; break; }
            }
            if (start == n.switch_cases.size()) start = default_index;
            if (start == n.switch_cases.size()) return Completion::Norm();
            for (size_t i = start; i < n.switch_cases.size(); ++i) {
                for (const auto &statement : n.switch_cases[i].second) {
                    Completion result = ExecStmt(interp, *statement, env);
                    if (result.type == CompletionType::Break) {
                        if (result.label.empty()) return Completion::Norm();
                        return result;
                    }
                    if (result.type != CompletionType::Normal) return result;
                }
            }
            return Completion::Norm();
        }
        case NodeKind::Label: {
            Completion result = ExecStmt(interp, *n.then_branch, env);
            if (result.type == CompletionType::Break && result.label == n.name) return Completion::Norm();
            if (result.type == CompletionType::Continue && result.label == n.name)
                return Completion::Thr("continue label does not name a loop");
            return result;
        }
        case NodeKind::Throw: {
            Completion value = EvalExpr(interp, *n.a, env);
            if (value.IsAbrupt()) return value;
            return Completion::Thr(value.value);
        }
        case NodeKind::Try: {
            Completion result = ExecStmt(interp, *n.then_branch, env);
            if (result.type == CompletionType::Throw && n.else_branch) {
                EnvPtr catch_scope = std::make_shared<Environment>();
                catch_scope->parent = env;
                if (!n.name.empty()) catch_scope->Define(n.name, interp.WrapThrown(result.value));
                result = ExecStmt(interp, *n.else_branch, catch_scope);
            }
            if (n.c) {
                Completion finalizer = ExecStmt(interp, *n.c, env);
                if (finalizer.IsAbrupt()) return finalizer;
            }
            return result;
        }
        case NodeKind::Return: {
            if (!n.a) return Completion::Ret(Value::Undef());
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            return Completion::Ret(c.value);
        }
        case NodeKind::Break:
            return Completion::Brk(n.name);
        case NodeKind::Continue:
            return Completion::Cont(n.name);
        default:
            return EvalExpr(interp, n, env);
    }
}

/**
 * @brief Wraps a native C++ callback as a callable JS function object.
 * @param fn The native callback, moved into the resulting object.
 * @return A Value wrapping an ObjectData with is_function set and `native` holding fn.
 */
Value MakeNativeFn(NativeFn fn) {
    auto obj = std::make_shared<ObjectData>();
    obj->is_function = true;
    obj->native = std::move(fn);
    return Value::Obj(obj);
}

std::unique_ptr<DomNode> TakeDomNode(HtmlDoc &doc, DomNode *node) {
    if (!node) return nullptr;
    auto take_from = [node](std::vector<std::unique_ptr<DomNode>> &nodes) {
        for (auto it = nodes.begin(); it != nodes.end(); ++it) if (it->get() == node) {
            std::unique_ptr<DomNode> result = std::move(*it); nodes.erase(it); return result;
        }
        return std::unique_ptr<DomNode>{};
    };
    if (std::unique_ptr<DomNode> detached = take_from(doc.detached_nodes)) return detached;
    return node->parent ? take_from(node->parent->children) : nullptr;
}

std::unique_ptr<DomNode> CloneDomNode(const DomNode *source, bool deep) {
    if (!source) return nullptr;
    auto clone = std::make_unique<DomNode>();
    clone->type = source->type; clone->tag = source->tag; clone->text = source->text; clone->attrs = source->attrs;
    clone->form_value = source->form_value; clone->form_checked = source->form_checked;
    clone->form_disabled = source->form_disabled; clone->details_open = source->details_open;
    clone->media_paused = source->media_paused; clone->media_muted = source->media_muted;
    clone->media_current_time = source->media_current_time; clone->media_volume = source->media_volume;
    if (source->shadow_root) {
        clone->shadow_root = CloneDomNode(source->shadow_root.get(), true);
        clone->shadow_root->parent = clone.get();
    }
    if (deep) for (const auto &child : source->children) {
        std::unique_ptr<DomNode> copied = CloneDomNode(child.get(), true);
        copied->parent = clone.get(); clone->children.push_back(std::move(copied));
    }
    return clone;
}

// Structural validation of a WebAssembly binary: magic/version, well-formed
// LEB128 section sizes that fit the buffer, custom-section names, the
// spec's mandatory section order, one occurrence per known section, and a
// function/code count match. It deliberately does not type-check function
// bodies -- that is the job of a real runtime, which mep does not have --
// so validate() answers "is this a plausibly loadable module" rather than
// "is this fully valid"; pages feature-detecting WASM get an honest yes/no
// on real binaries while garbage is still rejected.
bool ValidateWasmModuleStructure(const std::vector<unsigned char> &bytes) {
    static const unsigned char kHeader[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    if (bytes.size() < sizeof(kHeader) || !std::equal(kHeader, kHeader + sizeof(kHeader), bytes.begin())) return false;
    auto read_leb = [&bytes](size_t &pos, size_t end, uint32_t &out) {
        out = 0; unsigned shift = 0;
        while (pos < end && shift < 35) {
            unsigned char byte = bytes[pos++];
            out |= (byte & 0x7fU) << shift;
            if (!(byte & 0x80U)) return true;
            shift += 7;
        }
        return false;
    };
    // Section ids in the order the spec requires them to appear.
    static const unsigned char kOrder[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 10, 11};
    int last_rank = -1;
    uint32_t function_count = 0, code_count = 0;
    bool has_functions = false, has_code = false;
    size_t pos = sizeof(kHeader);
    while (pos < bytes.size()) {
        unsigned char id = bytes[pos++];
        uint32_t size = 0;
        if (!read_leb(pos, bytes.size(), size) || size > bytes.size() - pos) return false;
        size_t end = pos + size;
        if (id == 0) {
            uint32_t name_len = 0;
            if (!read_leb(pos, end, name_len) || name_len > end - pos) return false;
        } else {
            const unsigned char *rank = std::find(kOrder, kOrder + sizeof(kOrder), id);
            if (rank == kOrder + sizeof(kOrder)) return false;
            int this_rank = static_cast<int>(rank - kOrder);
            if (this_rank <= last_rank) return false;
            last_rank = this_rank;
            if (id == 3 || id == 10) {
                size_t count_pos = pos; uint32_t count = 0;
                if (!read_leb(count_pos, end, count)) return false;
                if (id == 3) { function_count = count; has_functions = true; } else { code_count = count; has_code = true; }
            }
        }
        pos = end;
    }
    if (pos != bytes.size()) return false;
    if (has_functions != has_code && (function_count > 0 || code_count > 0)) return false;
    return function_count == code_count;
}

bool DomHasClass(const DomNode *node, const std::string &want) {
    if (!node) return false;
    std::istringstream words(node->Class()); std::string word;
    while (words >> word) if (word == want) return true;
    return false;
}

Value MakeNodeList(HtmlDoc &doc, const std::vector<DomNode *> &nodes) {
    auto array = std::make_shared<ObjectData>();
    array->is_array = true;
    for (size_t i = 0; i < nodes.size(); ++i) array->props[std::to_string(i)] = WrapDomNode(doc, nodes[i]);
    array->props["length"] = Value::Num(static_cast<double>(nodes.size()));
    return Value::Obj(array);
}

void CollectDescendants(DomNode *root, const std::function<bool(DomNode *)> &accept, std::vector<DomNode *> &out) {
    for (const auto &child : root->children) {
        if (child->type == DomNodeType::Element && accept(child.get())) out.push_back(child.get());
        CollectDescendants(child.get(), accept, out);
    }
}

// getElementsByTagName / getElementsByClassName under `root` (static snapshot).
Value ElementsBy(HtmlDoc &doc, DomNode *root, bool by_class, const std::string &what) {
    std::vector<DomNode *> found;
    if (root) {
        const std::string tag = LowerAscii(what);
        CollectDescendants(root, [&](DomNode *candidate) {
            if (!by_class) return what == "*" || LowerAscii(candidate->tag) == tag;
            std::istringstream wanted(what);
            std::string word;
            bool any = false;
            while (wanted >> word) { any = true; if (!DomHasClass(candidate, word)) return false; }
            return any;
        }, found);
    }
    return MakeNodeList(doc, found);
}

bool DomMatchesSelector(DomNode *node, const std::string &selector) {
    DomNode *top = node;
    while (top->parent) top = top->parent;
    std::vector<DomNode *> matches = QuerySelectorAll(top, selector);
    return std::find(matches.begin(), matches.end(), node) != matches.end();
}

// Builds an Event object the way `new Event(type, {bubbles})` would.
Value MakeSyntheticEvent(const std::string &type, bool bubbles, bool cancelable) {
    auto event = std::make_shared<ObjectData>();
    event->props["type"] = Value::Str(type);
    event->props["bubbles"] = Value::Bool(bubbles);
    event->props["cancelable"] = Value::Bool(cancelable);
    event->props["isTrusted"] = Value::Bool(false);
    return Value::Obj(event);
}

// Dispatches a synthetic event at `node`; false when a listener called preventDefault().
bool FireDomEvent(HtmlDoc &doc, DomNode *node, const std::string &type, bool bubbles, bool &threw, std::string &error) {
    if (!g_active_interp) return true;
    std::vector<Value> args{MakeSyntheticEvent(type, bubbles, true)};
    Value result = DispatchDomEvent(*g_active_interp, &doc, node, args, threw, error);
    return threw ? false : result.Truthy();
}

// element.click(): the click event, then the control's default action.
bool ActivateDomNode(HtmlDoc &doc, DomNode *node, bool &threw, std::string &error) {
    const std::string type = node->attrs.count("type") ? LowerAscii(node->attrs["type"]) : "";
    const bool checkbox = node->tag == "input" && type == "checkbox";
    const bool radio = node->tag == "input" && type == "radio";
    const bool was_checked = node->form_checked;
    // A checkbox flips before its listeners run (they read the new state)
    // and flips back if one of them cancels the click.
    if (checkbox) node->form_checked = !node->form_checked;
    if (radio) node->form_checked = true;
    const bool proceed = FireDomEvent(doc, node, "click", true, threw, error);
    if (threw) return false;
    if (!proceed) { if (checkbox || radio) node->form_checked = was_checked; return false; }
    if (checkbox || radio) {
        FireDomEvent(doc, node, "input", true, threw, error);
        if (!threw) FireDomEvent(doc, node, "change", true, threw, error);
        return true;
    }
    // A submit control submits its form (cancelable through the submit event).
    DomNode *submitter = nullptr;
    for (DomNode *cur = node; cur && !submitter; cur = cur->parent) {
        const std::string cur_type = cur->attrs.count("type") ? LowerAscii(cur->attrs["type"]) : "";
        if ((cur->tag == "button" && (cur_type.empty() || cur_type == "submit")) || (cur->tag == "input" && cur_type == "submit")) submitter = cur;
    }
    if (!submitter || submitter->attrs.count("disabled")) return true;
    for (DomNode *form = submitter->parent; form; form = form->parent) {
        if (form->tag == "form") { FireDomEvent(doc, form, "submit", true, threw, error); return true; }
    }
    return true;
}

Value WrapDomNode(HtmlDoc &doc, DomNode *node) {
    if (!node) return Value::MakeNull();
    auto event_state = GetDomEventState(doc);
    if (auto cached = event_state->wrappers.find(node); cached != event_state->wrappers.end()) return Value::Obj(cached->second);
    auto wrapper = std::make_shared<ObjectData>();
    event_state->wrappers[node] = wrapper;
    wrapper->dom_node = node;
    wrapper->owner_doc = &doc;
    {
        // Which DOM interface this node implements decides its prototype,
        // so `el instanceof HTMLInputElement` and prototype accessors work.
        static const std::unordered_map<std::string, const char *> kInterfaces = {
            {"input", "HTMLInputElement"}, {"select", "HTMLSelectElement"}, {"textarea", "HTMLTextAreaElement"}, {"form", "HTMLFormElement"},
            {"button", "HTMLButtonElement"}, {"a", "HTMLAnchorElement"}, {"img", "HTMLImageElement"}, {"iframe", "HTMLIFrameElement"},
            {"option", "HTMLOptionElement"}, {"canvas", "HTMLCanvasElement"}, {"svg", "SVGElement"}, {"#document-fragment", "DocumentFragment"}};
        const char *interface_name = node->type != DomNodeType::Element ? "Text" : "HTMLElement";
        if (auto found = kInterfaces.find(node->tag); found != kInterfaces.end()) interface_name = found->second;
        if (auto proto = event_state->interface_protos.find(interface_name); proto != event_state->interface_protos.end()) wrapper->prototype = proto->second;
    }
    if (event_state->document_object) { wrapper->props["ownerDocument"] = Value::Obj(event_state->document_object); wrapper->non_enumerable.insert("ownerDocument"); }
    wrapper->props["click"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &, bool &threw, std::string &error) {
        ActivateDomNode(*doc_ptr, node, threw, error);
        return Value::Undef();
    });
    wrapper->props["matches"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && DomMatchesSelector(node, ToDisplayString(args[0])));
    });
    wrapper->props["closest"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::MakeNull();
        const std::string selector = ToDisplayString(args[0]);
        DomNode *top = node;
        while (top->parent) top = top->parent;
        std::vector<DomNode *> matches = QuerySelectorAll(top, selector);
        for (DomNode *cur = node; cur; cur = cur->parent) {
            if (cur->type == DomNodeType::Element && std::find(matches.begin(), matches.end(), cur) != matches.end()) return WrapDomNode(*doc_ptr, cur);
        }
        return Value::MakeNull();
    });
    wrapper->props["contains"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->dom_node) return Value::Bool(false);
        for (DomNode *cur = args[0].obj->dom_node; cur; cur = cur->parent) if (cur == node) return Value::Bool(true);
        return Value::Bool(false);
    });
    wrapper->props["getElementsByTagName"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        return ElementsBy(*doc_ptr, node, false, args.empty() ? "*" : ToDisplayString(args[0]));
    });
    wrapper->props["getElementsByClassName"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        return ElementsBy(*doc_ptr, node, true, args.empty() ? "" : ToDisplayString(args[0]));
    });
    wrapper->props["hasChildNodes"] = MakeNativeFn([node](const std::vector<Value> &, bool &, std::string &) { return Value::Bool(!node->children.empty()); });
    wrapper->props["getBoundingClientRect"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) {
        auto rect = std::make_shared<ObjectData>();
        for (const char *name : {"x", "y", "top", "left", "right", "bottom", "width", "height"}) rect->props[name] = Value::Num(0);
        return Value::Obj(rect);
    });
    if (node->type == DomNodeType::Element) {
        auto dataset = std::make_shared<ObjectData>();
        dataset->dataset_node = node;
        wrapper->props["dataset"] = Value::Obj(dataset);
    }
    wrapper->props["focus"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &, bool &, std::string &) {
        std::function<void(DomNode *)> clear_focus = [&](DomNode *current) {
            if (!current) return;
            current->interaction_focus = false;
            for (const auto &child : current->children) clear_focus(child.get());
            if (current->shadow_root) clear_focus(current->shadow_root.get());
        };
        clear_focus(doc_ptr->root.get());
        node->interaction_focus = true;
        return Value::Undef();
    });
    wrapper->props["blur"] = MakeNativeFn([node](const std::vector<Value> &, bool &, std::string &) { node->interaction_focus = false; return Value::Undef(); });
    wrapper->props["addEventListener"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.size() < 2 || args[0].type != VType::String || args[1].type != VType::Object || !args[1].obj || !args[1].obj->is_function) {
            threw = true; error = "addEventListener requires type and callback"; return Value::Undef();
        }
        bool capture = false, once = false;
        if (args.size() > 2) {
            if (args[2].type == VType::Boolean) capture = args[2].boolean;
            else if (args[2].type == VType::Object && args[2].obj) {
                Value configured_capture = GetProp(args[2].obj, "capture"), configured_once = GetProp(args[2].obj, "once");
                capture = configured_capture.Truthy(); once = configured_once.Truthy();
            }
        }
        auto &listeners = GetDomEventState(*doc_ptr)->listeners[node][args[0].str];
        for (const DomEventListener &listener : listeners) if (listener.callback == args[1].obj && listener.capture == capture) return Value::Undef();
        listeners.push_back({args[1].obj, capture, once});
        return Value::Undef();
    });
    wrapper->props["removeEventListener"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        if (args.size() < 2 || args[0].type != VType::String || args[1].type != VType::Object || !args[1].obj) return Value::Undef();
        bool capture = args.size() > 2 && args[2].type == VType::Boolean && args[2].boolean;
        auto state = GetDomEventState(*doc_ptr);
        auto by_type = state->listeners.find(node); if (by_type == state->listeners.end()) return Value::Undef();
        auto entries = by_type->second.find(args[0].str); if (entries == by_type->second.end()) return Value::Undef();
        auto &listeners = entries->second;
        listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [&](const DomEventListener &listener) { return listener.callback == args[1].obj && listener.capture == capture; }), listeners.end());
        return Value::Undef();
    });
    wrapper->props["attachShadow"] = MakeNativeFn([doc_ptr = &doc, node](std::vector<Value> &args, bool &threw, std::string &error) {
        if (node->type != DomNodeType::Element) { threw = true; error = "attachShadow requires an element"; return Value::Undef(); }
        if (node->shadow_root) { threw = true; error = "shadow root already attached"; return Value::Undef(); }
        if (!args.empty() && args[0].type != VType::Object) { threw = true; error = "attachShadow requires options"; return Value::Undef(); }
        node->shadow_root = std::make_unique<DomNode>(); node->shadow_root->tag = "#shadow-root"; node->shadow_root->parent = node;
        return WrapDomNode(*doc_ptr, node->shadow_root.get());
    });
    if (node->tag == "canvas") {
        wrapper->props["getContext"] = MakeNativeFn([node](std::vector<Value> &args, bool &, std::string &) {
            if (args.empty() || ToDisplayString(args[0]) != "2d") return Value::MakeNull();
            auto context = std::make_shared<ObjectData>();
            context->is_canvas_context = true;
            context->canvas_node = node;
            ObjectData *raw_context = context.get();
            // Geometry is transformed by the current transformation matrix
            // (CTM) as it is recorded, exactly like a real canvas: a later
            // translate()/rotate() never moves pixels already painted.
            auto apply = [raw_context](float x, float y, float &ox, float &oy) {
                const float *m = raw_context->canvas_transform;
                ox = m[0] * x + m[2] * y + m[4];
                oy = m[1] * x + m[3] * y + m[5];
            };
            auto length_scale = [raw_context]() {
                const float *m = raw_context->canvas_transform;
                return std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2]));
            };
            auto axis_aligned = [raw_context]() { return raw_context->canvas_transform[1] == 0.0f && raw_context->canvas_transform[2] == 0.0f; };
            auto paint = [raw_context](CanvasCommand &command, bool stroke) {
                const std::shared_ptr<ObjectData> &gradient = stroke ? raw_context->canvas_stroke_gradient : raw_context->canvas_fill_gradient;
                if (gradient && gradient->is_canvas_gradient) {
                    command.gradient = gradient->gradient;
                    command.gradient.present = true;
                    for (CanvasGradientStop &stop : command.gradient.stops) stop.a = static_cast<unsigned char>(static_cast<float>(stop.a) * raw_context->canvas_global_alpha);
                }
                command.r = stroke ? raw_context->canvas_stroke_r : raw_context->canvas_r;
                command.g = stroke ? raw_context->canvas_stroke_g : raw_context->canvas_g;
                command.b = stroke ? raw_context->canvas_stroke_b : raw_context->canvas_b;
                command.a = static_cast<unsigned char>(static_cast<float>(stroke ? raw_context->canvas_stroke_a : raw_context->canvas_a) * raw_context->canvas_global_alpha);
            };
            auto push_point = [raw_context, apply](float x, float y) {
                float tx, ty; apply(x, y, tx, ty);
                if (raw_context->canvas_subpaths.empty()) raw_context->canvas_subpaths.emplace_back();
                raw_context->canvas_subpaths.back().push_back(tx); raw_context->canvas_subpaths.back().push_back(ty);
            };
            auto current_point = [raw_context](float &x, float &y) {
                if (raw_context->canvas_subpaths.empty() || raw_context->canvas_subpaths.back().size() < 2) return false;
                const std::vector<float> &sub = raw_context->canvas_subpaths.back();
                x = sub[sub.size() - 2]; y = sub.back(); return true;
            };
            auto record = [node, raw_context, apply, length_scale, axis_aligned, paint](CanvasCommand::Kind kind, std::vector<Value> &values) {
                if (values.size() < 4) return Value::Undef();
                float x = static_cast<float>(ToNumber(values[0])), y = static_cast<float>(ToNumber(values[1]));
                float w = static_cast<float>(ToNumber(values[2])), h = static_cast<float>(ToNumber(values[3]));
                CanvasCommand command;
                command.kind = kind;
                command.line_width = raw_context->canvas_line_width * length_scale();
                paint(command, kind == CanvasCommand::Kind::StrokeRect);
                if (axis_aligned()) {
                    float x0, y0, x1, y1; apply(x, y, x0, y0); apply(x + w, y + h, x1, y1);
                    command.x = std::min(x0, x1); command.y = std::min(y0, y1); command.w = std::fabs(x1 - x0); command.h = std::fabs(y1 - y0);
                } else {
                    // A rotated/skewed rectangle is a general quadrilateral:
                    // record it as path geometry instead of a box.
                    std::vector<float> corners;
                    for (auto [cx, cy] : {std::pair<float, float>{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}) { float tx, ty; apply(cx, cy, tx, ty); corners.push_back(tx); corners.push_back(ty); }
                    if (kind == CanvasCommand::Kind::ClearRect) {
                        float min_x = corners[0], min_y = corners[1], max_x = corners[0], max_y = corners[1];
                        for (size_t i = 0; i < corners.size(); i += 2) { min_x = std::min(min_x, corners[i]); max_x = std::max(max_x, corners[i]); min_y = std::min(min_y, corners[i + 1]); max_y = std::max(max_y, corners[i + 1]); }
                        command.x = min_x; command.y = min_y; command.w = max_x - min_x; command.h = max_y - min_y;
                    } else {
                        command.kind = kind == CanvasCommand::Kind::FillRect ? CanvasCommand::Kind::FillPath : CanvasCommand::Kind::StrokePath;
                        if (kind == CanvasCommand::Kind::StrokeRect) { corners.push_back(corners[0]); corners.push_back(corners[1]); }
                        else command.triangles = TriangulateSvgPolygon(corners);
                        command.points = std::move(corners);
                    }
                }
                // A full-bitmap clear has a simple exact representation and
                // prevents an unbounded command list for animation loops.
                if (command.kind == CanvasCommand::Kind::ClearRect && command.x <= 0 && command.y <= 0 &&
                    command.w >= static_cast<float>(node->canvas_width) && command.h >= static_cast<float>(node->canvas_height)) node->canvas_commands.clear();
                else node->canvas_commands.push_back(std::move(command));
                return Value::Undef();
            };
            context->props["fillRect"] = MakeNativeFn([record](std::vector<Value> &values, bool &, std::string &) { return record(CanvasCommand::Kind::FillRect, values); });
            context->props["strokeRect"] = MakeNativeFn([record](std::vector<Value> &values, bool &, std::string &) { return record(CanvasCommand::Kind::StrokeRect, values); });
            context->props["clearRect"] = MakeNativeFn([record](std::vector<Value> &values, bool &, std::string &) { return record(CanvasCommand::Kind::ClearRect, values); });
            context->props["beginPath"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) { raw_context->canvas_subpaths.clear(); return Value::Undef(); });
            context->props["save"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) {
                std::vector<float> state = {static_cast<float>(raw_context->canvas_r), static_cast<float>(raw_context->canvas_g), static_cast<float>(raw_context->canvas_b), static_cast<float>(raw_context->canvas_a),
                                            static_cast<float>(raw_context->canvas_stroke_r), static_cast<float>(raw_context->canvas_stroke_g), static_cast<float>(raw_context->canvas_stroke_b), static_cast<float>(raw_context->canvas_stroke_a),
                                            raw_context->canvas_line_width, raw_context->canvas_font_size, raw_context->canvas_global_alpha};
                state.insert(state.end(), raw_context->canvas_transform, raw_context->canvas_transform + 6);
                raw_context->canvas_state_stack.push_back(std::move(state));
                raw_context->canvas_gradient_stack.emplace_back(raw_context->canvas_fill_gradient, raw_context->canvas_stroke_gradient);
                return Value::Undef();
            });
            context->props["restore"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) {
                if (raw_context->canvas_state_stack.empty()) return Value::Undef();
                const std::vector<float> state = raw_context->canvas_state_stack.back(); raw_context->canvas_state_stack.pop_back();
                raw_context->canvas_r = static_cast<unsigned char>(state[0]); raw_context->canvas_g = static_cast<unsigned char>(state[1]); raw_context->canvas_b = static_cast<unsigned char>(state[2]); raw_context->canvas_a = static_cast<unsigned char>(state[3]);
                raw_context->canvas_stroke_r = static_cast<unsigned char>(state[4]); raw_context->canvas_stroke_g = static_cast<unsigned char>(state[5]); raw_context->canvas_stroke_b = static_cast<unsigned char>(state[6]); raw_context->canvas_stroke_a = static_cast<unsigned char>(state[7]);
                raw_context->canvas_line_width = state[8]; raw_context->canvas_font_size = state[9]; raw_context->canvas_global_alpha = state[10];
                std::copy(state.begin() + 11, state.begin() + 17, raw_context->canvas_transform);
                if (!raw_context->canvas_gradient_stack.empty()) { raw_context->canvas_fill_gradient = raw_context->canvas_gradient_stack.back().first; raw_context->canvas_stroke_gradient = raw_context->canvas_gradient_stack.back().second; raw_context->canvas_gradient_stack.pop_back(); }
                return Value::Undef();
            });
            // --- transforms ---
            auto multiply = [raw_context](float a, float b, float c, float d, float e, float f) {
                float *m = raw_context->canvas_transform;
                float n[6] = {m[0] * a + m[2] * b, m[1] * a + m[3] * b, m[0] * c + m[2] * d, m[1] * c + m[3] * d, m[0] * e + m[2] * f + m[4], m[1] * e + m[3] * f + m[5]};
                std::copy(n, n + 6, m);
            };
            context->props["translate"] = MakeNativeFn([multiply](std::vector<Value> &values, bool &, std::string &) { if (values.size() >= 2) multiply(1, 0, 0, 1, static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1]))); return Value::Undef(); });
            context->props["scale"] = MakeNativeFn([multiply](std::vector<Value> &values, bool &, std::string &) { if (values.size() >= 2) multiply(static_cast<float>(ToNumber(values[0])), 0, 0, static_cast<float>(ToNumber(values[1])), 0, 0); return Value::Undef(); });
            context->props["rotate"] = MakeNativeFn([multiply](std::vector<Value> &values, bool &, std::string &) {
                if (values.empty()) return Value::Undef();
                float angle = static_cast<float>(ToNumber(values[0])), c = std::cos(angle), s = std::sin(angle);
                multiply(c, s, -s, c, 0, 0); return Value::Undef();
            });
            context->props["transform"] = MakeNativeFn([multiply](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 6) return Value::Undef();
                multiply(static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1])), static_cast<float>(ToNumber(values[2])), static_cast<float>(ToNumber(values[3])), static_cast<float>(ToNumber(values[4])), static_cast<float>(ToNumber(values[5])));
                return Value::Undef();
            });
            context->props["setTransform"] = MakeNativeFn([raw_context](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 6) return Value::Undef();
                for (size_t i = 0; i < 6; ++i) raw_context->canvas_transform[i] = static_cast<float>(ToNumber(values[i]));
                return Value::Undef();
            });
            context->props["resetTransform"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) {
                const float identity[6] = {1, 0, 0, 1, 0, 0}; std::copy(identity, identity + 6, raw_context->canvas_transform); return Value::Undef();
            });
            context->props["getTransform"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) {
                auto matrix = std::make_shared<ObjectData>();
                const char *names[6] = {"a", "b", "c", "d", "e", "f"};
                for (size_t i = 0; i < 6; ++i) matrix->props[names[i]] = Value::Num(static_cast<double>(raw_context->canvas_transform[i]));
                return Value::Obj(matrix);
            });
            // --- gradients ---
            auto make_gradient = [](bool radial, std::vector<Value> &values) {
                auto gradient = std::make_shared<ObjectData>();
                gradient->is_canvas_gradient = true;
                gradient->gradient.present = true;
                gradient->gradient.radial = radial;
                auto number_at = [&values](size_t i) { return i < values.size() ? static_cast<float>(ToNumber(values[i])) : 0.0f; };
                if (radial) { gradient->gradient.x0 = number_at(0); gradient->gradient.y0 = number_at(1); gradient->gradient.r0 = number_at(2); gradient->gradient.x1 = number_at(3); gradient->gradient.y1 = number_at(4); gradient->gradient.r1 = number_at(5); }
                else { gradient->gradient.x0 = number_at(0); gradient->gradient.y0 = number_at(1); gradient->gradient.x1 = number_at(2); gradient->gradient.y1 = number_at(3); }
                ObjectData *raw_gradient = gradient.get();
                gradient->props["addColorStop"] = MakeNativeFn([raw_gradient](std::vector<Value> &stop_args, bool &threw, std::string &error) {
                    if (stop_args.size() < 2) { threw = true; error = "addColorStop requires an offset and a color"; return Value::Undef(); }
                    double offset = ToNumber(stop_args[0]);
                    if (!(offset >= 0.0 && offset <= 1.0)) { threw = true; error = "IndexSizeError: gradient offset must be between 0 and 1"; return Value::Undef(); }
                    CanvasGradientStop stop; stop.offset = static_cast<float>(offset);
                    if (!ParseCssColor(ToDisplayString(stop_args[1]), stop.r, stop.g, stop.b, stop.a)) { threw = true; error = "SyntaxError: unrecognised gradient color"; return Value::Undef(); }
                    std::vector<CanvasGradientStop> &stops = raw_gradient->gradient.stops;
                    auto place = std::find_if(stops.begin(), stops.end(), [&stop](const CanvasGradientStop &existing) { return existing.offset > stop.offset; });
                    stops.insert(place, stop);
                    return Value::Undef();
                });
                return Value::Obj(gradient);
            };
            context->props["createLinearGradient"] = MakeNativeFn([make_gradient](std::vector<Value> &values, bool &, std::string &) { return make_gradient(false, values); });
            context->props["createRadialGradient"] = MakeNativeFn([make_gradient](std::vector<Value> &values, bool &, std::string &) { return make_gradient(true, values); });
            // --- path construction ---
            context->props["moveTo"] = MakeNativeFn([raw_context, push_point](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() >= 2) { raw_context->canvas_subpaths.emplace_back(); push_point(static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1]))); }
                return Value::Undef();
            });
            context->props["lineTo"] = MakeNativeFn([push_point](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() >= 2) push_point(static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1])));
                return Value::Undef();
            });
            context->props["rect"] = MakeNativeFn([raw_context, push_point](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 4) return Value::Undef();
                float x = static_cast<float>(ToNumber(values[0])), y = static_cast<float>(ToNumber(values[1])), w = static_cast<float>(ToNumber(values[2])), h = static_cast<float>(ToNumber(values[3]));
                raw_context->canvas_subpaths.emplace_back();
                push_point(x, y); push_point(x + w, y); push_point(x + w, y + h); push_point(x, y + h); push_point(x, y);
                return Value::Undef();
            });
            context->props["quadraticCurveTo"] = MakeNativeFn([apply, current_point, raw_context](std::vector<Value> &values, bool &, std::string &) {
                float x0, y0;
                if (values.size() < 4 || !current_point(x0, y0)) return Value::Undef();
                // Affine maps commute with Bezier evaluation, so flattening
                // in transformed space with transformed control points is exact.
                float cx, cy, x1, y1;
                apply(static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1])), cx, cy);
                apply(static_cast<float>(ToNumber(values[2])), static_cast<float>(ToNumber(values[3])), x1, y1);
                std::vector<float> &sub = raw_context->canvas_subpaths.back();
                for (int i = 1; i <= 12; ++i) { float t = static_cast<float>(i) / 12.0f, u = 1.0f - t; sub.push_back(u * u * x0 + 2.0f * u * t * cx + t * t * x1); sub.push_back(u * u * y0 + 2.0f * u * t * cy + t * t * y1); }
                return Value::Undef();
            });
            context->props["bezierCurveTo"] = MakeNativeFn([apply, current_point, raw_context](std::vector<Value> &values, bool &, std::string &) {
                float x0, y0;
                if (values.size() < 6 || !current_point(x0, y0)) return Value::Undef();
                float cx1, cy1, cx2, cy2, x1, y1;
                apply(static_cast<float>(ToNumber(values[0])), static_cast<float>(ToNumber(values[1])), cx1, cy1);
                apply(static_cast<float>(ToNumber(values[2])), static_cast<float>(ToNumber(values[3])), cx2, cy2);
                apply(static_cast<float>(ToNumber(values[4])), static_cast<float>(ToNumber(values[5])), x1, y1);
                std::vector<float> &sub = raw_context->canvas_subpaths.back();
                for (int i = 1; i <= 16; ++i) { float t = static_cast<float>(i) / 16.0f, u = 1.0f - t; sub.push_back(u * u * u * x0 + 3.0f * u * u * t * cx1 + 3.0f * u * t * t * cx2 + t * t * t * x1); sub.push_back(u * u * u * y0 + 3.0f * u * u * t * cy1 + 3.0f * u * t * t * cy2 + t * t * t * y1); }
                return Value::Undef();
            });
            context->props["closePath"] = MakeNativeFn([raw_context](std::vector<Value> &, bool &, std::string &) {
                if (raw_context->canvas_subpaths.empty()) return Value::Undef();
                std::vector<float> &sub = raw_context->canvas_subpaths.back();
                if (sub.size() >= 4) { sub.push_back(sub[0]); sub.push_back(sub[1]); }
                return Value::Undef();
            });
            context->props["arc"] = MakeNativeFn([push_point](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 5) return Value::Undef();
                float x = static_cast<float>(ToNumber(values[0])), y = static_cast<float>(ToNumber(values[1])), radius = std::max(0.0f, static_cast<float>(ToNumber(values[2])));
                float start = static_cast<float>(ToNumber(values[3])), end = static_cast<float>(ToNumber(values[4])); bool anticlockwise = values.size() > 5 && values[5].Truthy(); const float tau = 6.283185307179586f;
                if (!anticlockwise) while (end < start) end += tau; else while (end > start) end -= tau;
                int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(end - start) / (tau / 24.0f))));
                for (int i = 0; i <= segments; ++i) { float angle = start + (end - start) * static_cast<float>(i) / static_cast<float>(segments); push_point(x + radius * std::cos(angle), y + radius * std::sin(angle)); }
                return Value::Undef();
            });
            context->props["ellipse"] = MakeNativeFn([push_point](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 7) return Value::Undef();
                float x = static_cast<float>(ToNumber(values[0])), y = static_cast<float>(ToNumber(values[1])), rx = std::max(0.0f, static_cast<float>(ToNumber(values[2]))), ry = std::max(0.0f, static_cast<float>(ToNumber(values[3]))), rotation = static_cast<float>(ToNumber(values[4]));
                float start = static_cast<float>(ToNumber(values[5])), end = static_cast<float>(ToNumber(values[6])); bool anticlockwise = values.size() > 7 && values[7].Truthy(); const float tau = 6.283185307179586f;
                if (!anticlockwise) while (end < start) end += tau; else while (end > start) end -= tau;
                int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(end - start) / (tau / 24.0f)))); float cosine = std::cos(rotation), sine = std::sin(rotation);
                for (int i = 0; i <= segments; ++i) { float angle = start + (end - start) * static_cast<float>(i) / static_cast<float>(segments), ex = rx * std::cos(angle), ey = ry * std::sin(angle); push_point(x + ex * cosine - ey * sine, y + ex * sine + ey * cosine); }
                return Value::Undef();
            });
            context->props["stroke"] = MakeNativeFn([node, raw_context, paint, length_scale](std::vector<Value> &, bool &, std::string &) {
                for (const std::vector<float> &sub : raw_context->canvas_subpaths) {
                    if (sub.size() < 4) continue;
                    CanvasCommand command; command.kind = CanvasCommand::Kind::StrokePath; command.points = sub;
                    paint(command, true); command.line_width = raw_context->canvas_line_width * length_scale();
                    node->canvas_commands.push_back(std::move(command));
                }
                return Value::Undef();
            });
            context->props["fill"] = MakeNativeFn([node, raw_context, paint](std::vector<Value> &, bool &, std::string &) {
                for (const std::vector<float> &sub : raw_context->canvas_subpaths) {
                    if (sub.size() < 6) continue;
                    CanvasCommand command; command.kind = CanvasCommand::Kind::FillPath; command.points = sub;
                    command.triangles = TriangulateSvgPolygon(sub);
                    paint(command, false);
                    node->canvas_commands.push_back(std::move(command));
                }
                return Value::Undef();
            });
            context->props["isPointInPath"] = MakeNativeFn([raw_context](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 2) return Value::Bool(false);
                float x = static_cast<float>(ToNumber(values[0])), y = static_cast<float>(ToNumber(values[1]));
                for (const std::vector<float> &sub : raw_context->canvas_subpaths) {
                    if (sub.size() < 6) continue;
                    bool inside = false;
                    const size_t count = sub.size() / 2;
                    for (size_t i = 0, j = count - 1; i < count; j = i++) {
                        float xi = sub[i * 2], yi = sub[i * 2 + 1], xj = sub[j * 2], yj = sub[j * 2 + 1];
                        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
                    }
                    if (inside) return Value::Bool(true);
                }
                return Value::Bool(false);
            });
            context->props["fillText"] = MakeNativeFn([node, raw_context, apply, length_scale, paint](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 3) return Value::Undef();
                CanvasCommand command; command.kind = CanvasCommand::Kind::FillText;
                command.text = ToDisplayString(values[0]);
                apply(static_cast<float>(ToNumber(values[1])), static_cast<float>(ToNumber(values[2])), command.x, command.y);
                command.font_size = raw_context->canvas_font_size * length_scale();
                paint(command, false);
                node->canvas_commands.push_back(std::move(command)); return Value::Undef();
            });
            context->props["measureText"] = MakeNativeFn([raw_context](std::vector<Value> &values, bool &, std::string &) {
                auto metrics = std::make_shared<ObjectData>(); std::string text = values.empty() ? "" : ToDisplayString(values[0]);
                size_t glyphs = 0; for (char character : text) { unsigned char byte = static_cast<unsigned char>(character); if ((byte & 0xc0U) != 0x80U) ++glyphs; }
                metrics->props["width"] = Value::Num(static_cast<double>(glyphs) * static_cast<double>(raw_context->canvas_font_size) * 0.6);
                return Value::Obj(metrics);
            });
            context->props["createImageData"] = MakeNativeFn([](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 2) return Value::Undef();
                int width = std::max(0, static_cast<int>(ToNumber(values[0]))), height = std::max(0, static_cast<int>(ToNumber(values[1])));
                auto image = std::make_shared<ObjectData>(); image->props["width"] = Value::Num(width); image->props["height"] = Value::Num(height);
                auto data = std::make_shared<ObjectData>(); data->is_array = true;
                const size_t length = static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
                data->props["length"] = Value::Num(static_cast<double>(length));
                for (size_t i = 0; i < length; ++i) data->props[std::to_string(i)] = Value::Num(0);
                image->props["data"] = Value::Obj(data); return Value::Obj(image);
            });
            context->props["putImageData"] = MakeNativeFn([node](std::vector<Value> &values, bool &, std::string &) {
                if (values.size() < 3 || values[0].type != VType::Object || !values[0].obj) return Value::Undef();
                Value width = GetProp(values[0].obj, "width"), height = GetProp(values[0].obj, "height"), data = GetProp(values[0].obj, "data");
                if (width.type != VType::Number || height.type != VType::Number || data.type != VType::Object || !data.obj || !data.obj->is_array) return Value::Undef();
                int w = std::max(0, static_cast<int>(width.num)), h = std::max(0, static_cast<int>(height.num));
                CanvasCommand command; command.kind = CanvasCommand::Kind::ImageData; command.x = static_cast<float>(ToNumber(values[1])); command.y = static_cast<float>(ToNumber(values[2])); command.w = static_cast<float>(w); command.h = static_cast<float>(h);
                const size_t length = static_cast<size_t>(w) * static_cast<size_t>(h) * 4U; command.pixels.reserve(length);
                for (size_t i = 0; i < length; ++i) command.pixels.push_back(static_cast<unsigned char>(std::max(0.0, std::min(255.0, ToNumber(GetProp(data.obj, std::to_string(i)))))));
                node->canvas_commands.push_back(std::move(command)); return Value::Undef();
            });
            return Value::Obj(context);
        });
    }
    if (node->tag == "audio" || node->tag == "video") {
        wrapper->props["play"] = MakeNativeFn([node](std::vector<Value> &, bool &, std::string &) {
            // Playing again after the end restarts from the beginning, like a real element.
            if (node->media_ended) { node->media_ended = false; node->media_current_time = 0.0; }
            node->media_paused = false; return Value::Undef();
        });
        wrapper->props["pause"] = MakeNativeFn([node](std::vector<Value> &, bool &, std::string &) { node->media_paused = true; return Value::Undef(); });
        wrapper->props["load"] = MakeNativeFn([node](std::vector<Value> &, bool &, std::string &) { node->media_paused = true; node->media_ended = false; node->media_current_time = 0.0; return Value::Undef(); });
        // Only what the in-tree pipeline decodes is reported as playable, so
        // feature-detecting pages pick a supported source honestly.
        wrapper->props["canPlayType"] = MakeNativeFn([node](std::vector<Value> &args, bool &, std::string &) {
            std::string type = args.empty() ? "" : ToDisplayString(args[0]);
            for (char &c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            type = type.substr(0, type.find(';'));
            bool wav = type == "audio/wav" || type == "audio/wave" || type == "audio/x-wav" || type == "audio/vnd.wave";
            return Value::Str(node->tag == "audio" && wav ? "probably" : "");
        });
    }
    wrapper->props["appendChild"] = MakeNativeFn([&doc, node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->dom_node) { threw = true; error = "appendChild requires a node"; return Value::Undef(); }
        DomNode *child = args[0].obj->dom_node;
        if (child == node) { threw = true; error = "cannot append node to itself"; return Value::Undef(); }
        std::unique_ptr<DomNode> owned = TakeDomNode(doc, child);
        if (!owned) { threw = true; error = "node is not attachable"; return Value::Undef(); }
        owned->parent = node; node->children.push_back(std::move(owned));
        return args[0];
    });
    wrapper->props["insertBefore"] = MakeNativeFn([&doc, node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->dom_node) { threw = true; error = "insertBefore requires a node"; return Value::Undef(); }
        if (args.size() < 2 || args[1].type == VType::Null) {
            std::vector<Value> one{args[0]}; return GetProp(WrapDomNode(doc, node).obj, "appendChild").obj->native(one, threw, error);
        }
        if (args[1].type != VType::Object || !args[1].obj || !args[1].obj->dom_node) { threw = true; error = "insertBefore reference is not a node"; return Value::Undef(); }
        DomNode *child = args[0].obj->dom_node, *before = args[1].obj->dom_node;
        auto position = std::find_if(node->children.begin(), node->children.end(), [before](const auto &candidate) { return candidate.get() == before; });
        if (position == node->children.end()) { threw = true; error = "reference is not a child"; return Value::Undef(); }
        std::unique_ptr<DomNode> owned = TakeDomNode(doc, child);
        if (!owned) { threw = true; error = "node is not attachable"; return Value::Undef(); }
        owned->parent = node; node->children.insert(position, std::move(owned)); return args[0];
    });
    wrapper->props["removeChild"] = MakeNativeFn([&doc, node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->dom_node || args[0].obj->dom_node->parent != node) { threw = true; error = "removeChild requires a child"; return Value::Undef(); }
        if (std::unique_ptr<DomNode> owned = TakeDomNode(doc, args[0].obj->dom_node)) doc.detached_nodes.push_back(std::move(owned));
        return args[0];
    });
    wrapper->props["replaceChild"] = MakeNativeFn([&doc, node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.size() < 2 || args[0].type != VType::Object || args[1].type != VType::Object || !args[0].obj || !args[1].obj || !args[0].obj->dom_node || !args[1].obj->dom_node || args[1].obj->dom_node->parent != node) { threw = true; error = "replaceChild requires new and old child nodes"; return Value::Undef(); }
        DomNode *old = args[1].obj->dom_node; auto position = std::find_if(node->children.begin(), node->children.end(), [old](const auto &candidate) { return candidate.get() == old; });
        std::unique_ptr<DomNode> replacement = TakeDomNode(doc, args[0].obj->dom_node);
        if (!replacement) { threw = true; error = "replacement is not attachable"; return Value::Undef(); }
        std::unique_ptr<DomNode> displaced = std::move(*position); *position = std::move(replacement); (*position)->parent = node; displaced->parent = nullptr; doc.detached_nodes.push_back(std::move(displaced));
        return args[1];
    });
    wrapper->props["cloneNode"] = MakeNativeFn([&doc, node](const std::vector<Value> &args, bool &, std::string &) {
        bool deep = !args.empty() && args[0].Truthy(); std::unique_ptr<DomNode> clone = CloneDomNode(node, deep);
        DomNode *raw = clone.get(); doc.detached_nodes.push_back(std::move(clone)); return WrapDomNode(doc, raw);
    });
    wrapper->props["remove"] = MakeNativeFn([&doc, node](const std::vector<Value> &, bool &, std::string &) {
        if (std::unique_ptr<DomNode> owned = TakeDomNode(doc, node)) doc.detached_nodes.push_back(std::move(owned));
        return Value::Undef();
    });
    wrapper->props["setAttribute"] = MakeNativeFn([node](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.size() < 2 || args[0].type != VType::String) { threw = true; error = "setAttribute requires name and value"; return Value::Undef(); }
        node->attrs[args[0].str] = ToDisplayString(args[1]); return Value::Undef();
    });
    wrapper->props["getAttribute"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::MakeNull();
        auto it = node->attrs.find(args[0].str); return it == node->attrs.end() ? Value::MakeNull() : Value::Str(it->second);
    });
    wrapper->props["removeAttribute"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        if (!args.empty() && args[0].type == VType::String) node->attrs.erase(args[0].str);
        return Value::Undef();
    });
    wrapper->props["hasAttribute"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::String && node->attrs.count(args[0].str) != 0);
    });
    wrapper->props["querySelector"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        return args.empty() ? Value::MakeNull() : WrapDomNode(*doc_ptr, QuerySelector(node, ToDisplayString(args[0])));
    });
    wrapper->props["querySelectorAll"] = MakeNativeFn([doc_ptr = &doc, node](const std::vector<Value> &args, bool &, std::string &) {
        return MakeNodeList(*doc_ptr, args.empty() ? std::vector<DomNode *>{} : QuerySelectorAll(node, ToDisplayString(args[0])));
    });
    {
        // append/prepend/replaceChildren/before/after/insertAdjacent*: all
        // expressed through this wrapper's own appendChild/insertBefore.
        ObjectData *self = wrapper.get();
        auto as_node = [doc_ptr = &doc](const Value &arg) {
            if (arg.type == VType::Object && arg.obj && arg.obj->dom_node) return arg;
            auto text = std::make_unique<DomNode>();
            text->type = DomNodeType::Text;
            text->text = ToDisplayString(arg);
            DomNode *raw = text.get();
            doc_ptr->detached_nodes.push_back(std::move(text));
            return WrapDomNode(*doc_ptr, raw);
        };
        auto insert_at = [doc_ptr = &doc](DomNode *parent, DomNode *before, const Value &child, bool &threw, std::string &error) {
            if (!parent) return;
            ObjectPtr parent_wrapper = WrapDomNode(*doc_ptr, parent).obj;
            std::vector<Value> call_args{child};
            if (before) { call_args.push_back(WrapDomNode(*doc_ptr, before)); parent_wrapper->props["insertBefore"].obj->native(call_args, threw, error); }
            else parent_wrapper->props["appendChild"].obj->native(call_args, threw, error);
        };
        wrapper->props["append"] = MakeNativeFn([node, as_node, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            for (const Value &arg : args) { insert_at(node, nullptr, as_node(arg), threw, error); if (threw) break; }
            return Value::Undef();
        });
        wrapper->props["prepend"] = MakeNativeFn([node, as_node, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            DomNode *first = node->children.empty() ? nullptr : node->children.front().get();
            for (const Value &arg : args) { insert_at(node, first, as_node(arg), threw, error); if (threw) break; }
            return Value::Undef();
        });
        wrapper->props["replaceChildren"] = MakeNativeFn([node, as_node, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            RetireChildren(node);
            for (const Value &arg : args) { insert_at(node, nullptr, as_node(arg), threw, error); if (threw) break; }
            return Value::Undef();
        });
        wrapper->props["before"] = MakeNativeFn([node, as_node, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            for (const Value &arg : args) { insert_at(node->parent, node, as_node(arg), threw, error); if (threw) break; }
            return Value::Undef();
        });
        wrapper->props["after"] = MakeNativeFn([node, as_node, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            if (!node->parent) return Value::Undef();
            DomNode *next = nullptr;
            const auto &siblings = node->parent->children;
            for (size_t i = 0; i + 1 < siblings.size(); ++i) if (siblings[i].get() == node) next = siblings[i + 1].get();
            for (const Value &arg : args) { insert_at(node->parent, next, as_node(arg), threw, error); if (threw) break; }
            return Value::Undef();
        });
        wrapper->props["insertAdjacentHTML"] = MakeNativeFn([node, doc_ptr = &doc, insert_at](const std::vector<Value> &args, bool &threw, std::string &error) {
            if (args.size() < 2) return Value::Undef();
            const std::string where = LowerAscii(ToDisplayString(args[0]));
            HtmlDoc fragment;
            ParseHtml(ToDisplayString(args[1]), fragment);
            if (!fragment.root) return Value::Undef();
            DomNode *parent = (where == "beforebegin" || where == "afterend") ? node->parent : node;
            DomNode *before = nullptr;
            if (where == "beforebegin") before = node;
            else if (where == "afterbegin") before = node->children.empty() ? nullptr : node->children.front().get();
            else if (where == "afterend" && node->parent) {
                const auto &siblings = node->parent->children;
                for (size_t i = 0; i + 1 < siblings.size(); ++i) if (siblings[i].get() == node) before = siblings[i + 1].get();
            }
            std::vector<std::unique_ptr<DomNode>> incoming = std::move(fragment.root->children);
            for (auto &child : incoming) {
                DomNode *raw = child.get();
                raw->parent = nullptr;
                doc_ptr->detached_nodes.push_back(std::move(child));
                insert_at(parent, before, WrapDomNode(*doc_ptr, raw), threw, error);
                if (threw) break;
            }
            return Value::Undef();
        });
        (void)self;
    }
    auto class_list = std::make_shared<ObjectData>();
    class_list->props["contains"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) { return Value::Bool(!args.empty() && args[0].type == VType::String && DomHasClass(node, args[0].str)); });
    class_list->props["add"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        std::string classes = node->Class(); for (const Value &arg : args) if (arg.type == VType::String && !DomHasClass(node, arg.str)) classes += (classes.empty() ? "" : " ") + arg.str; node->attrs["class"] = classes; return Value::Undef();
    });
    class_list->props["remove"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        std::unordered_set<std::string> remove; for (const Value &arg : args) if (arg.type == VType::String) remove.insert(arg.str);
        std::istringstream words(node->Class()); std::string word, classes; while (words >> word) if (!remove.count(word)) classes += (classes.empty() ? "" : " ") + word; node->attrs["class"] = classes; return Value::Undef();
    });
    class_list->props["toggle"] = MakeNativeFn([node](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::Bool(false);
        bool had = DomHasClass(node, args[0].str);
        if (had) { std::istringstream words(node->Class()); std::string word, classes; while (words >> word) if (word != args[0].str) classes += (classes.empty() ? "" : " ") + word; node->attrs["class"] = classes; }
        else node->attrs["class"] += (node->Class().empty() ? "" : " ") + args[0].str;
        return Value::Bool(!had);
    });
    wrapper->props["classList"] = Value::Obj(class_list);
    auto style = std::make_shared<ObjectData>();
    style->style_node = node;
    wrapper->props["style"] = Value::Obj(style);
    return Value::Obj(wrapper);
}

/**
 * @brief Populates a global scope with this engine's entire DOM/console binding surface: console.log, document (with getElementById and the magic .title property), and a bare inert window object.
 * @param global The scope to define the globals in.
 * @param doc The document that document.getElementById/.title operate against.
 * @param on_console_log Forwarded to console.log's native implementation, invoked with each call's space-joined, stringified arguments.
 */
void SetupGlobals(Interpreter &interp, HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log) {
    EnvPtr &global = interp.global;
    auto json_to_value = std::make_shared<std::function<Value(const Json &)>>();
    *json_to_value = [json_to_value](const Json &json) -> Value {
        if (json.is_null()) return Value::MakeNull();
        if (json.is_bool()) return Value::Bool(json.as_bool());
        if (json.is_number()) return Value::Num(json.as_double());
        if (json.is_string()) return Value::Str(json.as_string());
        auto object = std::make_shared<ObjectData>();
        if (json.is_array()) {
            object->is_array = true;
            for (size_t i = 0; i < json.items().size(); ++i) object->props[std::to_string(i)] = (*json_to_value)(json.items()[i]);
            object->props["length"] = Value::Num(static_cast<double>(json.items().size()));
        } else for (const auto &field : json.fields()) object->props[field.first] = (*json_to_value)(field.second);
        return Value::Obj(object);
    };
    auto value_to_json = std::make_shared<std::function<bool(const Value &, Json &)>>();
    *value_to_json = [value_to_json](const Value &value, Json &json) -> bool {
        switch (value.type) {
            case VType::Undefined: return false;
            case VType::Null: json = Json(); return true;
            case VType::Boolean: json = Json(value.boolean); return true;
            case VType::Number: if (!std::isfinite(value.num)) { json = Json(); return true; } json = Json(value.num); return true;
            case VType::String: json = Json(value.str); return true;
            case VType::Object: {
                if (!value.obj || value.obj->is_function) return false;
                json = value.obj->is_array ? Json::Array() : Json::Object();
                if (value.obj->is_array) for (long i = 0; i < ArrayLength(value.obj); ++i) { Json child; if ((*value_to_json)(GetProp(value.obj, std::to_string(i)), child)) json.push_back(std::move(child)); else json.push_back(Json()); }
                else for (const auto &field : value.obj->props) { Json child; if ((*value_to_json)(field.second, child)) json[field.first] = std::move(child); }
                return true;
            }
        }
        return false;
    };
    auto console = std::make_shared<ObjectData>();
    // console.log(...args): stringifies and space-joins its arguments and forwards the line to on_console_log.
    console->props["log"] = MakeNativeFn([&on_console_log](const std::vector<Value> &args, bool &, std::string &) {
        std::string line;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) line += " ";
            line += ToDisplayString(args[i]);
        }
        on_console_log(line);
        return Value::Undef();
    });
    for (const char *level : {"error", "warn", "info", "debug", "trace"}) {
        const std::string prefix = std::string("[") + level + "] ";
        console->props[level] = MakeNativeFn([&on_console_log, prefix](const std::vector<Value> &args, bool &, std::string &) {
            std::string line = prefix;
            for (size_t i = 0; i < args.size(); i++) {
                if (i) line += " ";
                line += ToDisplayString(args[i]);
                // An Error's own stack says where it came from.
                if (args[i].type == VType::Object && args[i].obj && args[i].obj->props.contains("stack") && args[i].obj->props.contains("message")) {
                    const std::string stack = ToDisplayString(GetProp(args[i].obj, "stack"));
                    if (stack.find('\n') != std::string::npos) line += "\n" + stack;
                }
            }
            on_console_log(line);
            return Value::Undef();
        });
    }
    for (const char *ignored : {"group", "groupCollapsed", "groupEnd", "time", "timeEnd", "table", "assert", "count", "clear"}) {
        console->props[ignored] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return Value::Undef(); });
    }
    global->Define("console", Value::Obj(console));
    global->Define("queueMicrotask", MakeNativeFn([&interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) {
            threw = true;
            error = "queueMicrotask requires a function";
            return Value::Undef();
        }
        interp.microtasks.push_back(args[0].obj);
        return Value::Undef();
    }));
    auto schedule_timer = [&interp](bool repeat) {
        return MakeNativeFn([&interp, repeat](const std::vector<Value> &args, bool &threw, std::string &error) {
            if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) {
                threw = true; error = "timer requires a function"; return Value::Undef();
            }
            const double requested = args.size() > 1 ? ToNumber(args[1]) : 0;
            const long delay = std::isfinite(requested) ? std::max(0L, static_cast<long>(requested)) : 0;
            Interpreter::Timer timer;
            timer.id = interp.next_timer_id++; timer.callback = args[0].obj; timer.repeat = repeat;
            timer.interval = std::chrono::milliseconds(delay);
            timer.deadline = std::chrono::steady_clock::now() + timer.interval;
            for (size_t i = 2; i < args.size(); ++i) timer.args.push_back(args[i]);
            interp.timers.push_back(std::move(timer));
            return Value::Num(static_cast<double>(interp.next_timer_id - 1));
        });
    };
    global->Define("setTimeout", schedule_timer(false));
    global->Define("setInterval", schedule_timer(true));
    auto clear_timer = [&interp](const std::vector<Value> &args, bool &, std::string &) {
        if (!args.empty()) for (auto &timer : interp.timers) if (timer.id == static_cast<long>(ToNumber(args[0]))) timer.canceled = true;
        return Value::Undef();
    };
    global->Define("clearTimeout", MakeNativeFn(clear_timer));
    global->Define("clearInterval", MakeNativeFn(clear_timer));
    global->Define("requestAnimationFrame", MakeNativeFn([&interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) { threw = true; error = "requestAnimationFrame requires a function"; return Value::Undef(); }
        Interpreter::Timer timer; timer.id = interp.next_timer_id++; timer.callback = args[0].obj; timer.animation_frame = true;
        timer.deadline = std::chrono::steady_clock::now(); interp.timers.push_back(std::move(timer));
        return Value::Num(static_cast<double>(interp.next_timer_id - 1));
    }));
    global->Define("cancelAnimationFrame", MakeNativeFn(clear_timer));
    global->Define("Event", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty()) { threw = true; error = "Event requires a type"; return Value::Undef(); }
        auto event = std::make_shared<ObjectData>();
        event->props["type"] = Value::Str(ToDisplayString(args[0]));
        event->props["bubbles"] = Value::Bool(false);
        event->props["cancelable"] = Value::Bool(false);
        if (args.size() > 1 && args[1].type == VType::Object && args[1].obj) {
            event->props["bubbles"] = Value::Bool(GetProp(args[1].obj, "bubbles").Truthy());
            event->props["cancelable"] = Value::Bool(GetProp(args[1].obj, "cancelable").Truthy());
        }
        return Value::Obj(event);
    }));
    global->Define("CustomEvent", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty()) { threw = true; error = "CustomEvent requires a type"; return Value::Undef(); }
        auto event = std::make_shared<ObjectData>();
        event->props["type"] = Value::Str(ToDisplayString(args[0]));
        event->props["bubbles"] = Value::Bool(false);
        event->props["cancelable"] = Value::Bool(false);
        event->props["detail"] = Value::MakeNull();
        if (args.size() > 1 && args[1].type == VType::Object && args[1].obj) {
            event->props["bubbles"] = Value::Bool(GetProp(args[1].obj, "bubbles").Truthy());
            event->props["cancelable"] = Value::Bool(GetProp(args[1].obj, "cancelable").Truthy());
            Value detail = GetProp(args[1].obj, "detail"); if (detail.type != VType::Undefined) event->props["detail"] = detail;
        }
        return Value::Obj(event);
    }));
    global->Define("MouseEvent", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty()) { threw = true; error = "MouseEvent requires a type"; return Value::Undef(); }
        auto event = std::make_shared<ObjectData>();
        event->props["type"] = Value::Str(ToDisplayString(args[0]));
        event->props["bubbles"] = Value::Bool(false);
        event->props["cancelable"] = Value::Bool(false);
        event->props["clientX"] = Value::Num(0); event->props["clientY"] = Value::Num(0);
        event->props["button"] = Value::Num(0); event->props["ctrlKey"] = Value::Bool(false); event->props["shiftKey"] = Value::Bool(false);
        if (args.size() > 1 && args[1].type == VType::Object && args[1].obj) {
            for (const char *name : {"clientX", "clientY", "button", "ctrlKey", "shiftKey", "bubbles", "cancelable"}) {
                Value value = GetProp(args[1].obj, name); if (value.type != VType::Undefined) event->props[name] = value;
            }
        }
        return Value::Obj(event);
    }));
    global->Define("KeyboardEvent", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty()) { threw = true; error = "KeyboardEvent requires a type"; return Value::Undef(); }
        auto event = std::make_shared<ObjectData>();
        event->props["type"] = Value::Str(ToDisplayString(args[0]));
        event->props["bubbles"] = Value::Bool(false); event->props["cancelable"] = Value::Bool(false);
        event->props["key"] = Value::Str(""); event->props["code"] = Value::Str("");
        event->props["ctrlKey"] = Value::Bool(false); event->props["shiftKey"] = Value::Bool(false); event->props["altKey"] = Value::Bool(false); event->props["metaKey"] = Value::Bool(false);
        if (args.size() > 1 && args[1].type == VType::Object && args[1].obj) {
            for (const char *name : {"key", "code", "ctrlKey", "shiftKey", "altKey", "metaKey", "bubbles", "cancelable"}) {
                Value value = GetProp(args[1].obj, name); if (value.type != VType::Undefined) event->props[name] = value;
            }
        }
        return Value::Obj(event);
    }));
    auto promise_ctor = std::make_shared<ObjectData>();
    promise_ctor->is_function = true;
    promise_ctor->native = [&interp](std::vector<Value> &args, bool &threw, std::string &error) {
        auto promise = std::make_shared<ObjectData>();
        promise->is_promise = true;
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_function) {
            threw = true; error = "Promise resolver is not a function"; return Value::Undef();
        }
        ObjectPtr resolve = MakeNativeFn([&interp, promise](const std::vector<Value> &values, bool &, std::string &) {
            SettlePromise(interp, promise, 1, values.empty() ? Value::Undef() : values[0]); return Value::Undef();
        }).obj;
        ObjectPtr reject = MakeNativeFn([&interp, promise](const std::vector<Value> &values, bool &, std::string &) {
            SettlePromise(interp, promise, 2, values.empty() ? Value::Undef() : values[0]); return Value::Undef();
        }).obj;
        std::vector<Value> executor_args{Value::Obj(resolve), Value::Obj(reject)};
        Completion ran = CallFunction(interp, args[0].obj, executor_args);
        if (ran.type == CompletionType::Throw) SettlePromise(interp, promise, 2, ran.value);
        return Value::Obj(promise);
    };
    auto promise_settled = [&interp](int state) {
        return MakeNativeFn([&interp, state](const std::vector<Value> &args, bool &, std::string &) {
            auto promise = std::make_shared<ObjectData>(); promise->is_promise = true;
            SettlePromise(interp, promise, state, args.empty() ? Value::Undef() : args[0]);
            return Value::Obj(promise);
        });
    };
    promise_ctor->props["resolve"] = promise_settled(1);
    promise_ctor->props["reject"] = promise_settled(2);
    promise_ctor->props["all"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>(); result->is_promise = true;
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_array) {
            SettlePromise(interp, result, 2, Value::Str("Promise.all requires an array")); return Value::Obj(result);
        }
        const long length = ArrayLength(args[0].obj);
        auto values = std::make_shared<ObjectData>(); values->is_array = true; values->props["length"] = Value::Num(static_cast<double>(length));
        auto remaining = std::make_shared<long>(length);
        auto fulfill_at = [&interp, result, values, remaining](long index, Value value) {
            if (result->promise_state) return;
            values->props[std::to_string(index)] = std::move(value);
            if (--*remaining == 0) SettlePromise(interp, result, 1, Value::Obj(values));
        };
        if (length == 0) { SettlePromise(interp, result, 1, Value::Obj(values)); return Value::Obj(result); }
        for (long i = 0; i < length; ++i) {
            Value input = GetProp(args[0].obj, std::to_string(i));
            if (input.type != VType::Object || !input.obj || !input.obj->is_promise) { fulfill_at(i, input); continue; }
            ObjectPtr source = input.obj;
            ObjectPtr fulfilled = MakeNativeFn([fulfill_at, i](const std::vector<Value> &values, bool &, std::string &) { fulfill_at(i, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            ObjectPtr rejected = MakeNativeFn([&interp, result](const std::vector<Value> &values, bool &, std::string &) { SettlePromise(interp, result, 2, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            auto reaction = std::make_tuple(fulfilled, rejected, std::make_shared<ObjectData>());
            std::get<2>(reaction)->is_promise = true;
            if (source->promise_state) SchedulePromiseReaction(interp, source, reaction);
            else source->promise_reactions.push_back(std::move(reaction));
        }
        return Value::Obj(result);
    });
    promise_ctor->props["race"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>(); result->is_promise = true;
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_array) return Value::Obj(result);
        for (long i = 0; i < ArrayLength(args[0].obj); ++i) {
            Value input = GetProp(args[0].obj, std::to_string(i));
            if (input.type != VType::Object || !input.obj || !input.obj->is_promise) { SettlePromise(interp, result, 1, input); break; }
            ObjectPtr source = input.obj;
            ObjectPtr fulfilled = MakeNativeFn([&interp, result](const std::vector<Value> &values, bool &, std::string &) { SettlePromise(interp, result, 1, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            ObjectPtr rejected = MakeNativeFn([&interp, result](const std::vector<Value> &values, bool &, std::string &) { SettlePromise(interp, result, 2, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            auto reaction = std::make_tuple(fulfilled, rejected, std::make_shared<ObjectData>()); std::get<2>(reaction)->is_promise = true;
            if (source->promise_state) SchedulePromiseReaction(interp, source, reaction);
            else source->promise_reactions.push_back(std::move(reaction));
        }
        return Value::Obj(result);
    });
    promise_ctor->props["allSettled"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>(); result->is_promise = true;
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_array) {
            SettlePromise(interp, result, 2, Value::Str("Promise.allSettled requires an array")); return Value::Obj(result);
        }
        const long length = ArrayLength(args[0].obj);
        auto values = std::make_shared<ObjectData>(); values->is_array = true; values->props["length"] = Value::Num(static_cast<double>(length));
        auto remaining = std::make_shared<long>(length);
        auto record = [&interp, result, values, remaining](long index, int state, Value value) {
            if (result->promise_state) return;
            auto entry = std::make_shared<ObjectData>();
            entry->props["status"] = Value::Str(state == 1 ? "fulfilled" : "rejected");
            entry->props[state == 1 ? "value" : "reason"] = std::move(value);
            values->props[std::to_string(index)] = Value::Obj(entry);
            if (--*remaining == 0) SettlePromise(interp, result, 1, Value::Obj(values));
        };
        if (length == 0) { SettlePromise(interp, result, 1, Value::Obj(values)); return Value::Obj(result); }
        for (long i = 0; i < length; ++i) {
            Value input = GetProp(args[0].obj, std::to_string(i));
            if (input.type != VType::Object || !input.obj || !input.obj->is_promise) { record(i, 1, input); continue; }
            ObjectPtr source = input.obj;
            ObjectPtr fulfilled = MakeNativeFn([record, i](const std::vector<Value> &values, bool &, std::string &) { record(i, 1, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            ObjectPtr rejected = MakeNativeFn([record, i](const std::vector<Value> &values, bool &, std::string &) { record(i, 2, values.empty() ? Value::Undef() : values[0]); return Value::Undef(); }).obj;
            auto reaction = std::make_tuple(fulfilled, rejected, std::make_shared<ObjectData>()); std::get<2>(reaction)->is_promise = true;
            if (source->promise_state) SchedulePromiseReaction(interp, source, reaction);
            else source->promise_reactions.push_back(std::move(reaction));
        }
        return Value::Obj(result);
    });
    global->Define("Promise", Value::Obj(promise_ctor));
    global->Define("fetch", MakeNativeFn([&interp, &doc, json_to_value](const std::vector<Value> &args, bool &, std::string &) {
        auto promise = std::make_shared<ObjectData>(); promise->is_promise = true;
        auto make_headers = [](const std::string &url) {
            auto headers = std::make_shared<ObjectData>();
            std::string content_type = url.size() >= 5 && url.compare(url.size() - 5, 5, ".json") == 0 ? "application/json" : "text/plain";
            headers->props["content-type"] = Value::Str(content_type);
            headers->props["get"] = MakeNativeFn([headers](const std::vector<Value> &values, bool &, std::string &) { if (values.empty()) return Value::MakeNull(); std::string key = ToDisplayString(values[0]); for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); Value value = GetProp(headers, key); return value.type == VType::Undefined ? Value::MakeNull() : value; });
            headers->props["has"] = MakeNativeFn([headers](const std::vector<Value> &values, bool &, std::string &) { if (values.empty()) return Value::Bool(false); std::string key = ToDisplayString(values[0]); for (char &c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return Value::Bool(GetProp(headers, key).type != VType::Undefined); });
            headers->props["entries"] = MakeNativeFn([headers](const std::vector<Value> &, bool &, std::string &) { auto entries = std::make_shared<ObjectData>(); entries->is_array = true; auto pair = std::make_shared<ObjectData>(); pair->is_array = true; pair->props["0"] = Value::Str("content-type"); pair->props["1"] = GetProp(headers, "content-type"); pair->props["length"] = Value::Num(2); entries->props["0"] = Value::Obj(pair); entries->props["length"] = Value::Num(1); return Value::Obj(entries); });
            return headers;
        };
        if (args.empty() || args[0].type != VType::String || (doc.resource_base_dir.empty() && doc.resource_base_url.empty())) {
            SettlePromise(interp, promise, 2, Value::Str("fetch requires a local document resource")); return Value::Obj(promise);
        }
        const std::string url = args[0].str;
        if (!doc.resource_base_url.empty()) {
            // A network document: resolve against its URL and go through the
            // host's fetcher hook. Same-origin only -- the page may talk to
            // the server it came from, nothing else (no CORS machinery here).
            const std::string absolute = urlutil::ResolveUrl(doc.resource_base_url, url);
            const HtmlUrlFetcher &fetcher = GetHtmlUrlFetcher();
            if (!fetcher || !urlutil::SameOrigin(absolute, doc.resource_base_url)) {
                SettlePromise(interp, promise, 2, Value::Str("fetch URL is outside the document origin")); return Value::Obj(promise);
            }
            HtmlFetchResult fetched = fetcher(absolute);
            if (fetched.status == 0) {
                SettlePromise(interp, promise, 2, Value::Str("Failed to fetch: " + fetched.error)); return Value::Obj(promise);
            }
            auto response = std::make_shared<ObjectData>();
            response->props["ok"] = Value::Bool(fetched.status >= 200 && fetched.status < 300);
            response->props["status"] = Value::Num(fetched.status);
            response->props["statusText"] = Value::Str(fetched.status == 200 ? "OK" : (fetched.status == 404 ? "Not Found" : ""));
            response->props["url"] = Value::Str(fetched.url.empty() ? absolute : fetched.url);
            auto headers = make_headers(absolute);
            if (!fetched.content_type.empty()) headers->props["content-type"] = Value::Str(fetched.content_type);
            response->props["headers"] = Value::Obj(headers);
            const std::string body = std::move(fetched.body);
            response->props["text"] = MakeNativeFn([&interp, body](const std::vector<Value> &, bool &, std::string &) {
                auto result = std::make_shared<ObjectData>(); result->is_promise = true; SettlePromise(interp, result, 1, Value::Str(body)); return Value::Obj(result);
            });
            response->props["json"] = MakeNativeFn([&interp, body, json_to_value](const std::vector<Value> &, bool &, std::string &) {
                auto result = std::make_shared<ObjectData>(); result->is_promise = true;
                Json parsed;
                if (!Json::Parse(body, &parsed)) SettlePromise(interp, result, 2, Value::Str("invalid JSON response"));
                else SettlePromise(interp, result, 1, (*json_to_value)(parsed));
                return Value::Obj(result);
            });
            SettlePromise(interp, promise, 1, Value::Obj(response));
            return Value::Obj(promise);
        }
        std::filesystem::path path;
        if (!ResolveDocumentResource(doc.resource_base_dir, url, path)) {
            SettlePromise(interp, promise, 2, Value::Str("fetch URL is outside the document resource base")); return Value::Obj(promise);
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            auto response = std::make_shared<ObjectData>(); response->props["ok"] = Value::Bool(false); response->props["status"] = Value::Num(404); response->props["statusText"] = Value::Str("Not Found"); response->props["url"] = Value::Str(url); response->props["headers"] = Value::Obj(make_headers(url));
            SettlePromise(interp, promise, 1, Value::Obj(response)); return Value::Obj(promise);
        }
        std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto response = std::make_shared<ObjectData>(); response->props["ok"] = Value::Bool(true); response->props["status"] = Value::Num(200); response->props["statusText"] = Value::Str("OK"); response->props["url"] = Value::Str(url); response->props["headers"] = Value::Obj(make_headers(url));
        response->props["text"] = MakeNativeFn([&interp, body](const std::vector<Value> &, bool &, std::string &) {
            auto result = std::make_shared<ObjectData>(); result->is_promise = true; SettlePromise(interp, result, 1, Value::Str(body)); return Value::Obj(result);
        });
        response->props["json"] = MakeNativeFn([&interp, body, json_to_value](const std::vector<Value> &, bool &, std::string &) {
            auto result = std::make_shared<ObjectData>(); result->is_promise = true;
            Json parsed;
            if (!Json::Parse(body, &parsed)) SettlePromise(interp, result, 2, Value::Str("invalid JSON response"));
            else SettlePromise(interp, result, 1, (*json_to_value)(parsed));
            return Value::Obj(result);
        });
        SettlePromise(interp, promise, 1, Value::Obj(response));
        return Value::Obj(promise);
    }));
    global->Define("XMLHttpRequest", MakeNativeFn([&interp, &doc, json_to_value](const std::vector<Value> &, bool &, std::string &) {
        auto xhr = std::make_shared<ObjectData>();
        xhr->props["readyState"] = Value::Num(0); xhr->props["status"] = Value::Num(0);
        xhr->props["responseText"] = Value::Str(""); xhr->props["response"] = Value::MakeNull();
        xhr->props["responseType"] = Value::Str("");
        xhr->props["open"] = MakeNativeFn([xhr](const std::vector<Value> &args, bool &threw, std::string &error) {
            if (args.size() < 2) { threw = true; error = "XMLHttpRequest.open requires method and URL"; return Value::Undef(); }
            xhr->props["method"] = Value::Str(ToDisplayString(args[0])); xhr->props["url"] = Value::Str(ToDisplayString(args[1])); xhr->props["readyState"] = Value::Num(1);
            return Value::Undef();
        });
        xhr->props["setRequestHeader"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return Value::Undef(); });
        xhr->props["abort"] = MakeNativeFn([&interp, xhr](const std::vector<Value> &, bool &threw, std::string &error) {
            xhr->props["readyState"] = Value::Num(0); xhr->props["status"] = Value::Num(0);
            Value callback = GetProp(xhr, "onabort");
            if (callback.type == VType::Object && callback.obj && callback.obj->is_function) {
                std::vector<Value> callback_args; Completion result = CallFunction(interp, callback.obj, callback_args, nullptr);
                if (result.IsAbrupt()) { threw = true; error = ToDisplayString(result.value); }
            }
            return Value::Undef();
        });
        xhr->props["send"] = MakeNativeFn([&interp, &doc, xhr, json_to_value](const std::vector<Value> &, bool &threw, std::string &error) {
            Value url = GetProp(xhr, "url");
            std::filesystem::path path;
            if (url.type == VType::String && !doc.resource_base_url.empty()) {
                // Network document: same resolution and same-origin rule as fetch().
                const std::string absolute = urlutil::ResolveUrl(doc.resource_base_url, url.str);
                const HtmlUrlFetcher &fetcher = GetHtmlUrlFetcher();
                HtmlFetchResult fetched;
                if (fetcher && urlutil::SameOrigin(absolute, doc.resource_base_url)) fetched = fetcher(absolute);
                xhr->props["status"] = Value::Num(fetched.status); xhr->props["readyState"] = Value::Num(4);
                if (fetched.status != 0) {
                    xhr->props["responseText"] = Value::Str(fetched.body); xhr->props["response"] = Value::Str(fetched.body);
                    if (GetProp(xhr, "responseType").type == VType::String && GetProp(xhr, "responseType").str == "json") {
                        Json parsed; if (Json::Parse(fetched.body, &parsed)) xhr->props["response"] = (*json_to_value)(parsed);
                    }
                }
            } else if (url.type != VType::String || !ResolveDocumentResource(doc.resource_base_dir, url.str, path)) {
                xhr->props["status"] = Value::Num(0); xhr->props["readyState"] = Value::Num(4);
            } else {
                std::ifstream input(path, std::ios::binary);
                if (!input) { xhr->props["status"] = Value::Num(404); xhr->props["readyState"] = Value::Num(4); }
                else {
                    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
                    xhr->props["status"] = Value::Num(200); xhr->props["responseText"] = Value::Str(body); xhr->props["response"] = Value::Str(body); xhr->props["readyState"] = Value::Num(4);
                    if (GetProp(xhr, "responseType").type == VType::String && GetProp(xhr, "responseType").str == "json") {
                        Json parsed; if (Json::Parse(body, &parsed)) xhr->props["response"] = (*json_to_value)(parsed);
                    }
                }
            }
            auto notify = [&](const char *name) -> bool {
                Value callback = GetProp(xhr, name);
                if (callback.type != VType::Object || !callback.obj || !callback.obj->is_function) return true;
                std::vector<Value> callback_args;
                Completion result = CallFunction(interp, callback.obj, callback_args, nullptr);
                if (!result.IsAbrupt()) return true;
                threw = true; error = ToDisplayString(result.value); return false;
            };
            if (!notify("onreadystatechange")) return Value::Undef();
            if (GetProp(xhr, "status").type == VType::Number && GetProp(xhr, "status").num >= 200 && GetProp(xhr, "status").num < 300) notify("onload"); else notify("onerror");
            return Value::Undef();
        });
        return Value::Obj(xhr);
    }));

    global->Define("RegExp", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &err) {
        std::string pattern = args.empty() ? "" : ToDisplayString(args[0]);
        std::string flags = args.size() < 2 || args[1].type == VType::Undefined ? "" : ToDisplayString(args[1]);
        if (!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_regexp) {
            pattern = ToDisplayString(GetProp(args[0].obj, "source"));
            if (args.size() < 2 || args[1].type == VType::Undefined) flags = ToDisplayString(GetProp(args[0].obj, "flags"));
        }
        const std::string source = pattern;
        // Named groups become plain capturing groups for the matcher; the
        // names are kept by group number for exec()'s `groups`. Escapes the
        // matcher doesn't know (\/, \xHH, \uHHHH) are rewritten to literals.
        std::vector<std::string> group_names{""};
        {
            std::string plain;
            bool in_class = false;
            for (size_t i = 0; i < pattern.size(); ++i) {
                const char c = pattern[i];
                if (c == '\\' && i + 1 < pattern.size()) {
                    const char e = pattern[i + 1];
                    auto hex_at = [&](size_t from, size_t count, unsigned &out) {
                        if (from + count > pattern.size()) return false;
                        out = 0;
                        for (size_t k = 0; k < count; ++k) { const char h = pattern[from + k]; if (!std::isxdigit(static_cast<unsigned char>(h))) return false; out = out * 16 + static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : std::tolower(h) - 'a' + 10); }
                        return true;
                    };
                    unsigned cp = 0;
                    size_t used = 0;
                    if (e == 'x' && hex_at(i + 2, 2, cp)) used = 4;
                    else if (e == 'u' && hex_at(i + 2, 4, cp)) used = 6;
                    if (used) {
                        std::string utf8;
                        if (cp < 0x80) { if (std::strchr("\\^$.|?*+()[]{}-/", static_cast<int>(cp))) utf8 += '\\'; utf8 += static_cast<char>(cp); }
                        else if (cp < 0x800) { utf8 += static_cast<char>(0xC0 | (cp >> 6)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { utf8 += static_cast<char>(0xE0 | (cp >> 12)); utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); utf8 += static_cast<char>(0x80 | (cp & 0x3F)); }
                        plain += utf8;
                        i += used - 1;
                        continue;
                    }
                    if (e == '/') { plain += '/'; ++i; continue; }
                    plain += c; plain += e; ++i;
                    continue;
                }
                if (c == '[') in_class = true;
                else if (c == ']') in_class = false;
                if (!in_class && c == '(') {
                    const bool non_capturing = i + 1 < pattern.size() && pattern[i + 1] == '?';
                    const bool named = non_capturing && i + 2 < pattern.size() && pattern[i + 2] == '<' && i + 3 < pattern.size() && pattern[i + 3] != '=' && pattern[i + 3] != '!';
                    if (named) {
                        const size_t close = pattern.find('>', i + 3);
                        if (close != std::string::npos) { group_names.push_back(pattern.substr(i + 3, close - i - 3)); plain += '('; i = close; continue; }
                    }
                    if (!non_capturing) group_names.push_back("");
                }
                plain += c;
            }
            pattern = plain;
        }
        auto regex = std::make_shared<mep_regex::Regex>(pattern, flags.find('i') != std::string::npos);
        if (!regex->ok()) { threw = true; err = "invalid RegExp: " + regex->error(); return Value::Undef(); }
        auto value = std::make_shared<ObjectData>();
        value->is_regexp = true;
        value->regexp_global = flags.find('g') != std::string::npos;
        value->regexp_group_names = std::move(group_names);
        value->props["lastIndex"] = Value::Num(0);
        value->props["source"] = Value::Str(source);
        value->props["flags"] = Value::Str(flags);
        value->props["global"] = Value::Bool(value->regexp_global);
        value->props["ignoreCase"] = Value::Bool(flags.find('i') != std::string::npos);
        value->props["multiline"] = Value::Bool(flags.find('m') != std::string::npos);
        for (const char *hidden : {"lastIndex", "source", "flags", "global", "ignoreCase", "multiline"}) value->non_enumerable.insert(hidden);
        value->regexp = std::move(regex);
        return Value::Obj(value);
    }));

    auto make_collection = [](bool set) {
        return MakeNativeFn([set](const std::vector<Value> &args, bool &threw, std::string &err) {
            auto collection = std::make_shared<ObjectData>(); collection->is_map = !set; collection->is_set = set;
            if (!args.empty() && args[0].type != VType::Null && args[0].type != VType::Undefined) {
                if (args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_array) { threw = true; err = "collection initializer requires an array"; return Value::Undef(); }
                for (long i = 0; i < ArrayLength(args[0].obj); ++i) {
                    Value entry = GetProp(args[0].obj, std::to_string(i));
                    if (set) {
                        bool exists = std::any_of(collection->collection_entries.begin(), collection->collection_entries.end(), [&](const auto &existing) { return StrictEquals(existing.first, entry); });
                        if (!exists) collection->collection_entries.emplace_back(entry, entry);
                    }
                    else if (entry.type == VType::Object && entry.obj && entry.obj->is_array) collection->collection_entries.emplace_back(GetProp(entry.obj, "0"), GetProp(entry.obj, "1"));
                }
            }
            return Value::Obj(collection);
        });
    };
    global->Define("Map", make_collection(false));
    global->Define("Set", make_collection(true));
    // Values are reference-managed for the lifetime of a script today, so
    // WeakMap/WeakSet share Map/Set storage semantics while exposing the
    // standard weak-collection API surface.
    global->Define("WeakMap", make_collection(false));
    global->Define("WeakSet", make_collection(true));

    auto symbol = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        auto value = std::make_shared<ObjectData>(); value->is_symbol = true;
        value->props["description"] = Value::Str(args.empty() ? "" : ToDisplayString(args[0]));
        return Value::Obj(value);
    });
    for (const char *well_known : {"iterator", "asyncIterator", "hasInstance", "toStringTag", "toPrimitive", "species", "isConcatSpreadable", "unscopables"}) {
        auto known = std::make_shared<ObjectData>();
        known->is_symbol = true;
        known->symbol_key = std::string("@@") + well_known;
        known->props["description"] = Value::Str(std::string("Symbol.") + well_known);
        symbol.obj->props[well_known] = Value::Obj(known);
    }
    auto symbol_registry = std::make_shared<std::unordered_map<std::string, ObjectPtr>>();
    symbol.obj->props["for"] = MakeNativeFn([symbol_registry](const std::vector<Value> &args, bool &, std::string &) {
        const std::string name = args.empty() ? "undefined" : ToDisplayString(args[0]);
        ObjectPtr &entry = (*symbol_registry)[name];
        if (!entry) {
            entry = std::make_shared<ObjectData>();
            entry->is_symbol = true;
            entry->symbol_key = "@@for:" + name;
            entry->props["description"] = Value::Str(name);
        }
        return Value::Obj(entry);
    });
    symbol.obj->props["keyFor"] = MakeNativeFn([symbol_registry](const std::vector<Value> &args, bool &, std::string &) {
        if (!args.empty() && args[0].type == VType::Object) for (const auto &entry : *symbol_registry) if (entry.second == args[0].obj) return Value::Str(entry.first);
        return Value::Undef();
    });
    global->Define("Symbol", symbol);

    global->Define("Proxy", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &err) {
        if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj || args[1].type != VType::Object || !args[1].obj) { threw = true; err = "Proxy requires target and handler objects"; return Value::Undef(); }
        auto proxy = std::make_shared<ObjectData>(); proxy->proxy_target = args[0].obj; proxy->proxy_handler = args[1].obj;
        return Value::Obj(proxy);
    }));

    // Every error type shares Error.prototype at the root of its chain, so
    // `e instanceof Error` holds for a TypeError too.
    auto error_root = std::make_shared<ObjectData>();
    auto make_error = [error_root](const std::string &name) {
        ObjectPtr proto = error_root;
        if (name != "Error") { proto = std::make_shared<ObjectData>(); proto->prototype = error_root; }
        proto->props["name"] = Value::Str(name);
        Value ctor = MakeNativeFn([name, proto](const std::vector<Value> &args, bool &, std::string &) {
            auto error = std::make_shared<ObjectData>();
            error->prototype = proto;
            error->props["name"] = Value::Str(name);
            error->props["message"] = Value::Str(args.empty() ? "" : ToDisplayString(args[0]));
            error->props["stack"] = Value::Str(name + ": " + (args.empty() ? "" : ToDisplayString(args[0])));
            return Value::Obj(error);
        });
        ctor.obj->props["prototype"] = Value::Obj(proto);
        proto->props["constructor"] = ctor;
        return ctor;
    };
    global->Define("Error", make_error("Error"));
    global->Define("TypeError", make_error("TypeError"));
    global->Define("RangeError", make_error("RangeError"));
    global->Define("SyntaxError", make_error("SyntaxError"));
    global->Define("ReferenceError", make_error("ReferenceError"));
    global->Define("EvalError", make_error("EvalError"));
    global->Define("URIError", make_error("URIError"));
    {
        // Engine failures are raised as bare message strings; at a catch
        // site they become the Error object script code expects.
        Value error_ctor = *global->Find("Error");
        Value type_error_ctor = *global->Find("TypeError");
        Value reference_error_ctor = *global->Find("ReferenceError");
        interp.wrap_thrown = [error_ctor, type_error_ctor, reference_error_ctor](const Value &thrown) {
            const std::string &message = thrown.str;
            const Value &ctor = message.find("is not defined") != std::string::npos ? reference_error_ctor
                                : (message.find("is not a") != std::string::npos || message.find("not callable") != std::string::npos ||
                                   message.find("of non-object") != std::string::npos || message.find("of undefined") != std::string::npos ||
                                   message.find("of null") != std::string::npos)
                                    ? type_error_ctor
                                    : error_ctor;
            std::vector<Value> args{thrown};
            bool threw = false;
            std::string err;
            return ctor.obj->native(args, threw, err);
        };
    }

    global->Define("NaN", Value::Num(std::nan("")));
    global->Define("Infinity", Value::Num(std::numeric_limits<double>::infinity()));
    global->Define("isNaN", MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Bool(args.empty() || std::isnan(ToNumber(args[0]))); }));
    global->Define("isFinite", MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Bool(!args.empty() && std::isfinite(ToNumber(args[0]))); }));
    global->Define("String", MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Str(args.empty() ? "" : ToDisplayString(args[0])); }));
    global->Find("String")->obj->props["fromCharCode"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        std::string out;
        for (const Value &arg : args) {
            const uint32_t cp = static_cast<uint32_t>(ToNumber(arg));
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        }
        return Value::Str(out);
    });
    global->Define("Boolean", MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Bool(!args.empty() && args[0].Truthy()); }));
    global->Define("Function", MakeNativeFn([](const std::vector<Value> &, bool &threw, std::string &err) {
        threw = true; err = "Function() from source text is not supported"; return Value::Undef();
    }));
    auto uri_component = [](bool encode) {
        return MakeNativeFn([encode](const std::vector<Value> &args, bool &, std::string &) {
            const std::string in = args.empty() ? "undefined" : ToDisplayString(args[0]);
            std::string out;
            if (encode) {
                static const char *hex = "0123456789ABCDEF";
                for (char raw : in) {
                    const unsigned char c = static_cast<unsigned char>(raw);
                    if (std::isalnum(c) || std::string("-_.!~*'()").find(raw) != std::string::npos) out += raw;
                    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
                }
            } else {
                for (size_t i = 0; i < in.size(); ++i) {
                    if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) && std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
                        out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
                        i += 2;
                    } else out += in[i];
                }
            }
            return Value::Str(out);
        });
    };
    global->Define("encodeURIComponent", uri_component(true));
    global->Define("decodeURIComponent", uri_component(false));

    // Date: a time value in ms since the epoch; fields read in local time.
    {
        auto date_proto = std::make_shared<ObjectData>();
        auto now_ms = [] {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        };
        Value date_ctor = MakeNativeFn([date_proto, now_ms](const std::vector<Value> &args, bool &, std::string &) {
            auto date = std::make_shared<ObjectData>();
            date->is_date = true;
            date->prototype = date_proto;
            if (args.empty()) date->date_ms = now_ms();
            else if (args.size() == 1 && args[0].type == VType::String) {
                std::tm tm{};
                int ms = 0;
                const std::string &text = args[0].str;
                int y = 0, mo = 1, d = 1, h = 0, mi = 0, sec = 0;
                const int got = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d.%d", &y, &mo, &d, &h, &mi, &sec, &ms);
                if (got >= 3) {
                    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = sec;
                    const bool utc = got == 3 || (!text.empty() && text.back() == 'Z');
                    tm.tm_isdst = -1;
                    const std::time_t t = utc ? timegm(&tm) : mktime(&tm);
                    date->date_ms = static_cast<double>(t) * 1000.0 + ms;
                } else date->date_ms = std::nan("");
            } else if (args.size() == 1) date->date_ms = ToNumber(args[0]);
            else {
                std::tm tm{};
                tm.tm_year = static_cast<int>(ToNumber(args[0])) - 1900;
                tm.tm_mon = static_cast<int>(ToNumber(args[1]));
                tm.tm_mday = args.size() > 2 ? static_cast<int>(ToNumber(args[2])) : 1;
                tm.tm_hour = args.size() > 3 ? static_cast<int>(ToNumber(args[3])) : 0;
                tm.tm_min = args.size() > 4 ? static_cast<int>(ToNumber(args[4])) : 0;
                tm.tm_sec = args.size() > 5 ? static_cast<int>(ToNumber(args[5])) : 0;
                tm.tm_isdst = -1;
                date->date_ms = static_cast<double>(mktime(&tm)) * 1000.0 + (args.size() > 6 ? ToNumber(args[6]) : 0);
            }
            return Value::Obj(date);
        });
        date_ctor.obj->props["now"] = MakeNativeFn([now_ms](const std::vector<Value> &, bool &, std::string &) { return Value::Num(now_ms()); });
        date_ctor.obj->props["prototype"] = Value::Obj(date_proto);
        date_proto->props["constructor"] = date_ctor;
        global->Define("Date", date_ctor);
    }

    auto reflect = std::make_shared<ObjectData>();
    reflect->props["get"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return args.size() < 2 || args[0].type != VType::Object || !args[0].obj ? Value::Undef() : GetProp(args[0].obj, ToDisplayString(args[1])); });
    reflect->props["set"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { if (args.size() < 3 || args[0].type != VType::Object || !args[0].obj) return Value::Bool(false); SetProp(args[0].obj, ToDisplayString(args[1]), args[2]); return Value::Bool(true); });
    reflect->props["has"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj) return Value::Bool(false); return Value::Bool(GetProp(args[0].obj, ToDisplayString(args[1])).type != VType::Undefined); });
    reflect->props["deleteProperty"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj || args[0].obj->frozen) return Value::Bool(false); args[0].obj->props.erase(ToDisplayString(args[1])); return Value::Bool(true); });
    reflect->props["ownKeys"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { auto result = std::make_shared<ObjectData>(); result->is_array = true; if (!args.empty() && args[0].type == VType::Object && args[0].obj) { size_t index = 0; for (const auto &entry : args[0].obj->props) result->props[std::to_string(index++)] = Value::Str(entry.first); result->props["length"] = Value::Num(static_cast<double>(index)); } else result->props["length"] = Value::Num(0); return Value::Obj(result); });
    global->Define("Reflect", Value::Obj(reflect));

    auto json = std::make_shared<ObjectData>();
    json->props["parse"] = MakeNativeFn([json_to_value](const std::vector<Value> &args, bool &threw, std::string &err) {
        if (args.empty()) { threw = true; err = "JSON.parse requires text"; return Value::Undef(); }
        Json parsed;
        if (!Json::Parse(ToDisplayString(args[0]), &parsed)) { threw = true; err = "invalid JSON"; return Value::Undef(); }
        return (*json_to_value)(parsed);
    });
    (void)value_to_json;
    json->props["stringify"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty()) return Value::Undef();
        std::string indent;
        if (args.size() > 2) {
            if (args[2].type == VType::Number) indent.assign(static_cast<size_t>(std::max(0.0, std::min(10.0, args[2].num))), ' ');
            else if (args[2].type == VType::String) indent = args[2].str.substr(0, 10);
        }
        std::string out;
        return JsonStringify(interp, args[0], indent, "", out, threw, error) ? Value::Str(out) : Value::Undef();
    });
    global->Define("JSON", Value::Obj(json));

    auto array_ctor = std::make_shared<ObjectData>();
    array_ctor->props["isArray"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_array);
    });
    array_ctor->is_function = true;
    array_ctor->native = [](std::vector<Value> &args, bool &, std::string &) {
        // Array(n) makes n holes; any other argument list is the elements.
        if (args.size() == 1 && args[0].type == VType::Number) {
            auto array = std::make_shared<ObjectData>();
            array->is_array = true;
            array->props["length"] = Value::Num(std::max(0.0, std::floor(args[0].num)));
            return Value::Obj(array);
        }
        return MakeArray(args);
    };
    array_ctor->props["of"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return MakeArray(args); });
    array_ctor->props["from"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        std::vector<Value> items;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj && !args[0].obj->is_array && !args[0].obj->is_map && !args[0].obj->is_set &&
            !args[0].obj->is_list_iterator && GetProp(args[0].obj, "@@iterator").type == VType::Undefined && GetProp(args[0].obj, "next").type == VType::Undefined) {
            // Array-like: {length, 0: ..., 1: ...}
            const double length = ToNumber(GetProp(args[0].obj, "length"));
            for (long i = 0; i < static_cast<long>(std::isnan(length) ? 0 : length); ++i) items.push_back(GetProp(args[0].obj, std::to_string(i)));
        } else if (!args.empty() && args[0].type != VType::Undefined && args[0].type != VType::Null) {
            if (!IterateValues(interp, args[0], items, error)) { threw = true; return Value::Undef(); }
        }
        if (args.size() > 1 && args[1].type == VType::Object && args[1].obj && args[1].obj->is_function) {
            for (size_t i = 0; i < items.size(); ++i) {
                std::vector<Value> call_args{items[i], Value::Num(static_cast<double>(i))};
                Completion mapped = CallFunction(interp, args[1].obj, call_args);
                if (mapped.IsAbrupt()) { threw = true; error = ToDisplayString(mapped.value); return Value::Undef(); }
                items[i] = mapped.value;
            }
        }
        return MakeArray(items);
    });
    // X.prototype.method: re-enters the builtin member lookup with the
    // call's receiver, so `Array.prototype.slice.call(arguments, 1)` and
    // `String.prototype.trim.call(s)` reach the same code `a.slice(1)` does.
    auto make_shim_proto = [&interp](std::initializer_list<const char *> names) {
        auto proto = std::make_shared<ObjectData>();
        for (const char *name : names) {
            const std::string key = name;
            proto->props[key] = MakeNativeFn([&interp, key](std::vector<Value> &args, bool &threw, std::string &error) {
                const Value self = NativeThis();
                Completion method = MemberGet(interp, self, key);
                if (method.IsAbrupt() || method.value.type != VType::Object || !method.value.obj || !method.value.obj->is_function || !method.value.obj->native) {
                    threw = true; error = key + " called on an incompatible receiver"; return Value::Undef();
                }
                Completion called = CallFunction(interp, method.value.obj, args, &self);
                if (called.IsAbrupt()) { threw = true; error = ToDisplayString(called.value); return Value::Undef(); }
                return called.value;
            });
            proto->non_enumerable.insert(key);
        }
        return proto;
    };
    auto array_proto = make_shim_proto({"push", "pop", "shift", "unshift", "slice", "splice", "concat", "join", "reverse", "sort", "indexOf", "includes", "flat",
                                        "map", "filter", "forEach", "some", "every", "find", "findIndex", "reduce", "flatMap", "keys", "values", "entries"});
    array_ctor->props["prototype"] = Value::Obj(array_proto);
    array_proto->props["constructor"] = Value::Obj(array_ctor);
    array_proto->non_enumerable.insert("constructor");
    global->Define("Array", Value::Obj(array_ctor));
    {
        Value string_ctor = *global->Find("String");
        auto string_proto = make_shim_proto({"toUpperCase", "toLowerCase", "includes", "startsWith", "endsWith", "slice", "substring", "substr", "trim", "trimStart", "trimEnd",
                                             "repeat", "padStart", "padEnd", "split", "replace", "replaceAll", "match", "search", "indexOf", "lastIndexOf", "charAt",
                                             "charCodeAt", "codePointAt", "at", "concat", "localeCompare", "toString", "valueOf"});
        string_ctor.obj->props["prototype"] = Value::Obj(string_proto);
        string_proto->props["constructor"] = string_ctor;
        string_proto->non_enumerable.insert("constructor");
        Value function_ctor = *global->Find("Function");
        auto function_proto = make_shim_proto({"call", "apply", "bind"});
        function_ctor.obj->props["prototype"] = Value::Obj(function_proto);
        Value boolean_ctor = *global->Find("Boolean");
        boolean_ctor.obj->props["prototype"] = Value::Obj(std::make_shared<ObjectData>());
    }

    auto object_ctor = std::make_shared<ObjectData>();
    auto object_prototype = std::make_shared<ObjectData>();
    object_ctor->props["prototype"] = Value::Obj(object_prototype);
    auto make_array = [](const std::vector<Value> &values) {
        auto array = std::make_shared<ObjectData>();
        array->is_array = true;
        for (size_t i = 0; i < values.size(); ++i) array->props[std::to_string(i)] = values[i];
        array->props["length"] = Value::Num(static_cast<double>(values.size()));
        return Value::Obj(array);
    };
    object_ctor->props["keys"] = MakeNativeFn([make_array](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> values;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj)
            for (const std::string &key : EnumerableKeys(args[0].obj)) { Value value = GetProp(args[0].obj, key); (void)value; values.push_back(Value::Str(key)); }
        return make_array(values);
    });
    object_ctor->props["values"] = MakeNativeFn([make_array](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> values;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj)
            for (const std::string &key : EnumerableKeys(args[0].obj)) { Value value = GetProp(args[0].obj, key); (void)value; values.push_back(value); }
        return make_array(values);
    });
    object_ctor->props["entries"] = MakeNativeFn([make_array](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> entries;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj)
            for (const std::string &key : EnumerableKeys(args[0].obj)) { Value value = GetProp(args[0].obj, key); (void)value; entries.push_back(make_array({Value::Str(key), value})); }
        return make_array(entries);
    });
    object_ctor->props["assign"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj) return Value::Undef();
        for (size_t i = 1; i < args.size(); ++i) if (args[i].type == VType::Object && args[i].obj)
            for (const std::string &key : EnumerableKeys(args[i].obj)) SetProp(args[0].obj, key, GetProp(args[i].obj, key));
        return args[0];
    });
    object_ctor->props["create"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        auto object = std::make_shared<ObjectData>();
        if (!args.empty() && args[0].type == VType::Object) object->prototype = args[0].obj;
        return Value::Obj(object);
    });
    object_ctor->props["getPrototypeOf"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj) return Value::MakeNull();
        if (args[0].obj->prototype && !args[0].obj->is_class) return Value::Obj(args[0].obj->prototype);
        if (args[0].obj->is_class) return args[0].obj->super_class ? Value::Obj(args[0].obj->super_class) : BuiltinProtoLookupObject(interp, "Function");
        if (args[0].obj->is_array) return BuiltinProtoLookupObject(interp, "Array");
        if (args[0].obj->is_function) return BuiltinProtoLookupObject(interp, "Function");
        return Value::MakeNull();
    });
    object_ctor->props["setPrototypeOf"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj) return Value::Undef();
        args[0].obj->prototype = args[1].type == VType::Object ? args[1].obj : nullptr;
        return args[0];
    });
    object_ctor->props["freeze"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (!args.empty() && args[0].type == VType::Object && args[0].obj) args[0].obj->frozen = true;
        return args.empty() ? Value::Undef() : args[0];
    });
    object_ctor->props["defineProperty"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.size() < 3 || args[0].type != VType::Object || !args[0].obj) return Value::Undef();
        const std::string key = PropertyKey(args[1]);
        if (args[2].type == VType::Object && args[2].obj) {
            if (!GetProp(args[2].obj, "enumerable").Truthy()) args[0].obj->non_enumerable.insert(key);
            else args[0].obj->non_enumerable.erase(key);
            if (args[2].obj->props.count("value") && GetProp(args[2].obj, "value").type == VType::Undefined) args[0].obj->props[key] = Value::Undef();
            Value getter = GetProp(args[2].obj, "get"), setter = GetProp(args[2].obj, "set");
            if (getter.type == VType::Object && getter.obj && getter.obj->is_function) args[0].obj->getters[key] = getter.obj;
            if (setter.type == VType::Object && setter.obj && setter.obj->is_function) args[0].obj->setters[key] = setter.obj;
            Value value = GetProp(args[2].obj, "value");
            if (value.type != VType::Undefined) SetProp(args[0].obj, key, value);
        }
        return args[0];
    });
    object_ctor->props["defineProperties"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj || args[1].type != VType::Object || !args[1].obj) return Value::Undef();
        for (const auto &entry : args[1].obj->props) {
            if (entry.second.type != VType::Object || !entry.second.obj) continue;
            Value getter = GetProp(entry.second.obj, "get"), setter = GetProp(entry.second.obj, "set"), value = GetProp(entry.second.obj, "value");
            if (getter.type == VType::Object && getter.obj && getter.obj->is_function) args[0].obj->getters[entry.first] = getter.obj;
            if (setter.type == VType::Object && setter.obj && setter.obj->is_function) args[0].obj->setters[entry.first] = setter.obj;
            if (value.type != VType::Undefined) SetProp(args[0].obj, entry.first, value);
        }
        return args[0];
    });
    object_ctor->props["getOwnPropertyDescriptor"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj) return Value::Undef();
        const std::string key = ToDisplayString(args[1]);
        auto value = args[0].obj->props.find(key);
        auto getter = args[0].obj->getters.find(key), setter = args[0].obj->setters.find(key);
        if (value == args[0].obj->props.end() && getter == args[0].obj->getters.end() && setter == args[0].obj->setters.end()) return Value::Undef();
        auto descriptor = std::make_shared<ObjectData>();
        if (value != args[0].obj->props.end()) descriptor->props["value"] = value->second;
        if (getter != args[0].obj->getters.end()) descriptor->props["get"] = Value::Obj(getter->second);
        if (setter != args[0].obj->setters.end()) descriptor->props["set"] = Value::Obj(setter->second);
        descriptor->props["enumerable"] = Value::Bool(!args[0].obj->non_enumerable.count(key));
        descriptor->props["writable"] = Value::Bool(!args[0].obj->frozen);
        descriptor->props["configurable"] = Value::Bool(!args[0].obj->frozen);
        return Value::Obj(descriptor);
    });
    object_ctor->is_function = true;
    object_ctor->native = [](std::vector<Value> &args, bool &, std::string &) {
        if (!args.empty() && args[0].type == VType::Object && args[0].obj) return args[0];
        return Value::Obj(std::make_shared<ObjectData>());
    };
    object_ctor->props["fromEntries"] = MakeNativeFn([&interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        auto result = std::make_shared<ObjectData>();
        std::vector<Value> pairs;
        if (!args.empty() && !IterateValues(interp, args[0], pairs, error)) { threw = true; return Value::Undef(); }
        for (const Value &pair : pairs) if (pair.type == VType::Object && pair.obj) result->props[PropertyKey(GetProp(pair.obj, "0"))] = GetProp(pair.obj, "1");
        return Value::Obj(result);
    });
    object_ctor->props["getOwnPropertyNames"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> names;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj) {
            for (const auto &entry : args[0].obj->props) if (!IsHiddenKey(entry.first)) names.push_back(Value::Str(entry.first));
        }
        return MakeArray(names);
    });
    object_ctor->props["getOwnPropertySymbols"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return MakeArray({}); });
    object_ctor->props["is"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        const Value a = args.empty() ? Value::Undef() : args[0], b = args.size() < 2 ? Value::Undef() : args[1];
        if (a.type == VType::Number && b.type == VType::Number) {
            if (std::isnan(a.num) && std::isnan(b.num)) return Value::Bool(true);
            if (a.num == 0 && b.num == 0) return Value::Bool(std::signbit(a.num) == std::signbit(b.num));
        }
        return Value::Bool(StrictEquals(a, b));
    });
    object_ctor->props["isFrozen"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(args.empty() || args[0].type != VType::Object || !args[0].obj || args[0].obj->frozen);
    });
    object_ctor->props["isSealed"] = object_ctor->props["isFrozen"];
    object_ctor->props["seal"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return args.empty() ? Value::Undef() : args[0]; });
    object_ctor->props["preventExtensions"] = object_ctor->props["seal"];
    object_ctor->props["isExtensible"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Object && args[0].obj && !args[0].obj->frozen);
    });
    object_prototype->props["hasOwnProperty"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        const Value self = NativeThis();
        if (args.empty() || self.type != VType::Object || !self.obj) return Value::Bool(false);
        const std::string key = PropertyKey(args[0]);
        return Value::Bool(self.obj->props.contains(key) || self.obj->getters.count(key) || self.obj->setters.count(key));
    });
    object_prototype->props["propertyIsEnumerable"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        const Value self = NativeThis();
        if (args.empty() || self.type != VType::Object || !self.obj) return Value::Bool(false);
        const std::string key = PropertyKey(args[0]);
        return Value::Bool(self.obj->props.contains(key) && !self.obj->non_enumerable.count(key));
    });
    object_prototype->props["isPrototypeOf"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        const Value self = NativeThis();
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || self.type != VType::Object) return Value::Bool(false);
        for (ObjectPtr cur = args[0].obj->prototype; cur; cur = cur->prototype) if (cur == self.obj) return Value::Bool(true);
        return Value::Bool(false);
    });
    object_prototype->props["toString"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) {
        const Value self = NativeThis();
        switch (self.type) {
            case VType::Undefined: return Value::Str("[object Undefined]");
            case VType::Null: return Value::Str("[object Null]");
            case VType::Number: return Value::Str("[object Number]");
            case VType::String: return Value::Str("[object String]");
            case VType::Boolean: return Value::Str("[object Boolean]");
            case VType::Object: break;
        }
        if (!self.obj) return Value::Str("[object Object]");
        if (self.obj->is_array) return Value::Str("[object Array]");
        if (self.obj->is_function || self.obj->is_class) return Value::Str("[object Function]");
        if (self.obj->is_date) return Value::Str("[object Date]");
        if (self.obj->is_regexp) return Value::Str("[object RegExp]");
        if (self.obj->props.contains("message") && self.obj->props.contains("stack")) return Value::Str("[object Error]");
        return Value::Str("[object Object]");
    });
    object_prototype->props["valueOf"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return NativeThis(); });
    object_prototype->props["toLocaleString"] = object_prototype->props["toString"];
    for (const auto &entry : object_prototype->props) object_prototype->non_enumerable.insert(entry.first);
    object_prototype->props["constructor"] = Value::Obj(object_ctor);
    object_prototype->non_enumerable.insert("constructor");
    global->Define("Object", Value::Obj(object_ctor));

    auto math = std::make_shared<ObjectData>();
    math->props["abs"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? std::numeric_limits<double>::quiet_NaN() : std::fabs(ToNumber(args[0]))); });
    math->props["floor"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? std::numeric_limits<double>::quiet_NaN() : std::floor(ToNumber(args[0]))); });
    math->props["ceil"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? std::numeric_limits<double>::quiet_NaN() : std::ceil(ToNumber(args[0]))); });
    math->props["round"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? std::numeric_limits<double>::quiet_NaN() : std::floor(ToNumber(args[0]) + 0.5)); });
    math->props["min"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { double result = std::numeric_limits<double>::infinity(); for (const Value &arg : args) result = std::min(result, ToNumber(arg)); return Value::Num(result); });
    math->props["max"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { double result = -std::numeric_limits<double>::infinity(); for (const Value &arg : args) result = std::max(result, ToNumber(arg)); return Value::Num(result); });
    // The rest of the numeric Math surface canvas/animation code leans on.
    auto unary = [&math](const char *name, double (*fn)(double)) {
        math->props[name] = MakeNativeFn([fn](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? std::numeric_limits<double>::quiet_NaN() : fn(ToNumber(args[0]))); });
    };
    unary("sqrt", [](double v) { return std::sqrt(v); }); unary("cbrt", [](double v) { return std::cbrt(v); });
    unary("sin", [](double v) { return std::sin(v); }); unary("cos", [](double v) { return std::cos(v); }); unary("tan", [](double v) { return std::tan(v); });
    unary("asin", [](double v) { return std::asin(v); }); unary("acos", [](double v) { return std::acos(v); }); unary("atan", [](double v) { return std::atan(v); });
    unary("exp", [](double v) { return std::exp(v); }); unary("log", [](double v) { return std::log(v); }); unary("log2", [](double v) { return std::log2(v); }); unary("log10", [](double v) { return std::log10(v); });
    unary("trunc", [](double v) { return std::trunc(v); }); unary("sign", [](double v) { return std::isnan(v) ? v : (v > 0 ? 1.0 : (v < 0 ? -1.0 : v)); });
    math->props["atan2"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.size() < 2 ? std::numeric_limits<double>::quiet_NaN() : std::atan2(ToNumber(args[0]), ToNumber(args[1]))); });
    math->props["pow"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.size() < 2 ? std::numeric_limits<double>::quiet_NaN() : std::pow(ToNumber(args[0]), ToNumber(args[1]))); });
    math->props["hypot"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) { double sum = 0; for (const Value &arg : args) { double v = ToNumber(arg); sum += v * v; } return Value::Num(std::sqrt(sum)); });
    math->props["random"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return Value::Num(static_cast<double>(std::rand()) / (static_cast<double>(RAND_MAX) + 1.0)); });
    math->props["PI"] = Value::Num(3.141592653589793); math->props["E"] = Value::Num(2.718281828459045);
    math->props["SQRT2"] = Value::Num(1.4142135623730951); math->props["LN2"] = Value::Num(0.6931471805599453); math->props["LN10"] = Value::Num(2.302585092994046);
    global->Define("Math", Value::Obj(math));

    auto number_ctor = std::make_shared<ObjectData>();
    number_ctor->props["isNaN"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Number && std::isnan(args[0].num));
    });
    number_ctor->props["isFinite"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Number && std::isfinite(args[0].num));
    });
    number_ctor->props["isInteger"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Number && std::isfinite(args[0].num) && std::floor(args[0].num) == args[0].num);
    });
    number_ctor->props["parseInt"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::Num(std::numeric_limits<double>::quiet_NaN());
        std::string text = ToDisplayString(args[0]);
        int radix = args.size() > 1 ? static_cast<int>(ToNumber(args[1])) : 10;
        char *end = nullptr;
        long value = std::strtol(text.c_str(), &end, radix);
        return end == text.c_str() ? Value::Num(std::numeric_limits<double>::quiet_NaN()) : Value::Num(static_cast<double>(value));
    });
    number_ctor->props["parseFloat"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::Num(std::numeric_limits<double>::quiet_NaN());
        std::string text = ToDisplayString(args[0]);
        char *end = nullptr;
        double value = std::strtod(text.c_str(), &end);
        return end == text.c_str() ? Value::Num(std::numeric_limits<double>::quiet_NaN()) : Value::Num(value);
    });
    number_ctor->is_function = true;
    number_ctor->native = [](std::vector<Value> &args, bool &, std::string &) { return Value::Num(args.empty() ? 0 : ToNumber(args[0])); };
    number_ctor->props["MAX_SAFE_INTEGER"] = Value::Num(9007199254740991.0);
    number_ctor->props["MIN_SAFE_INTEGER"] = Value::Num(-9007199254740991.0);
    number_ctor->props["MAX_VALUE"] = Value::Num(std::numeric_limits<double>::max());
    number_ctor->props["MIN_VALUE"] = Value::Num(std::numeric_limits<double>::denorm_min());
    number_ctor->props["EPSILON"] = Value::Num(std::numeric_limits<double>::epsilon());
    number_ctor->props["POSITIVE_INFINITY"] = Value::Num(std::numeric_limits<double>::infinity());
    number_ctor->props["NEGATIVE_INFINITY"] = Value::Num(-std::numeric_limits<double>::infinity());
    number_ctor->props["NaN"] = Value::Num(std::nan(""));
    number_ctor->props["isSafeInteger"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Number && std::isfinite(args[0].num) && std::floor(args[0].num) == args[0].num && std::fabs(args[0].num) <= 9007199254740991.0);
    });
    number_ctor->props["prototype"] = Value::Obj(std::make_shared<ObjectData>());
    global->Define("Number", Value::Obj(number_ctor));
    global->Define("parseInt", number_ctor->props["parseInt"]);
    global->Define("parseFloat", number_ctor->props["parseFloat"]);

    // The registry is deliberately independent of individual elements.  The
    // current interpreter has no `new`/class semantics yet, but registering
    // and looking up a custom-element definition is still useful to library
    // bootstrap code and provides the stable base for upgrade callbacks.
    auto custom_elements = std::make_shared<ObjectData>();
    auto definitions = std::make_shared<std::unordered_map<std::string, Value>>();
    custom_elements->props["define"] = MakeNativeFn([definitions](std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.size() < 2 || args[0].type != VType::String || args[0].str.find('-') == std::string::npos) { threw = true; error = "customElements.define requires a hyphenated name and constructor"; return Value::Undef(); }
        if (definitions->count(args[0].str)) { threw = true; error = "custom element already defined"; return Value::Undef(); }
        (*definitions)[args[0].str] = args[1]; return Value::Undef();
    });
    custom_elements->props["get"] = MakeNativeFn([definitions](std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::Undef();
        auto it = definitions->find(args[0].str); return it == definitions->end() ? Value::Undef() : it->second;
    });
    global->Define("customElements", Value::Obj(custom_elements));

    auto web_assembly = std::make_shared<ObjectData>();
    web_assembly->props["validate"] = MakeNativeFn([](std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_array) return Value::Bool(false);
        std::vector<unsigned char> bytes;
        size_t length = static_cast<size_t>(std::max<long>(0, ArrayLength(args[0].obj)));
        bytes.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            Value byte = GetProp(args[0].obj, std::to_string(i));
            if (byte.type != VType::Number || byte.num < 0 || byte.num > 255 || byte.num != std::floor(byte.num)) return Value::Bool(false);
            bytes.push_back(static_cast<unsigned char>(byte.num));
        }
        return Value::Bool(ValidateWasmModuleStructure(bytes));
    });
    global->Define("WebAssembly", Value::Obj(web_assembly));

    auto document = std::make_shared<ObjectData>();
    document->is_document = true;
    document->owner_doc = &doc;
    // document.getElementById(id): finds the first element with matching id in doc's tree and wraps it, or returns null if none matches or the argument isn't a string.
    document->props["getElementById"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::MakeNull();
        DomNode *found = doc.root ? FindById(doc.root.get(), args[0].str) : nullptr;
        if (!found) return Value::MakeNull();
        return WrapDomNode(doc, found);
    });
    document->props["querySelector"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::MakeNull();
        DomNode *found = doc.root ? QuerySelector(doc.root.get(), args[0].str) : nullptr;
        if (!found) return Value::MakeNull();
        return WrapDomNode(doc, found);
    });
    document->props["querySelectorAll"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        auto array = std::make_shared<ObjectData>();
        array->is_array = true;
        if (args.empty() || args[0].type != VType::String || !doc.root) {
            array->props["length"] = Value::Num(0);
            return Value::Obj(array);
        }
        std::vector<DomNode *> matches = QuerySelectorAll(doc.root.get(), args[0].str);
        for (size_t i = 0; i < matches.size(); ++i) {
            array->props[std::to_string(i)] = WrapDomNode(doc, matches[i]);
        }
        array->props["length"] = Value::Num(static_cast<double>(matches.size()));
        return Value::Obj(array);
    });
    document->props["createElement"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.empty() || args[0].type != VType::String || args[0].str.empty()) { threw = true; error = "createElement requires a tag name"; return Value::Undef(); }
        auto node = std::make_unique<DomNode>(); node->type = DomNodeType::Element; node->tag = args[0].str;
        DomNode *raw = node.get(); doc.detached_nodes.push_back(std::move(node)); return WrapDomNode(doc, raw);
    });
    document->props["createElementNS"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (args.size() < 2 || args[1].type != VType::String || args[1].str.empty()) { threw = true; error = "createElementNS requires namespace and tag name"; return Value::Undef(); }
        auto node = std::make_unique<DomNode>(); node->type = DomNodeType::Element; node->tag = args[1].str;
        std::transform(node->tag.begin(), node->tag.end(), node->tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (args[0].type == VType::String) node->attrs["xmlns"] = args[0].str;
        DomNode *raw = node.get(); doc.detached_nodes.push_back(std::move(node)); return WrapDomNode(doc, raw);
    });
    document->props["createTextNode"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        auto node = std::make_unique<DomNode>(); node->type = DomNodeType::Text; node->text = args.empty() ? "" : ToDisplayString(args[0]);
        DomNode *raw = node.get(); doc.detached_nodes.push_back(std::move(node)); return WrapDomNode(doc, raw);
    });
    document->props["getElementsByTagName"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        return ElementsBy(doc, doc.root.get(), false, args.empty() ? "*" : ToDisplayString(args[0]));
    });
    document->props["getElementsByClassName"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        return ElementsBy(doc, doc.root.get(), true, args.empty() ? "" : ToDisplayString(args[0]));
    });
    document->props["createDocumentFragment"] = MakeNativeFn([&doc](const std::vector<Value> &, bool &, std::string &) {
        auto node = std::make_unique<DomNode>(); node->type = DomNodeType::Element; node->tag = "#document-fragment";
        DomNode *raw = node.get(); doc.detached_nodes.push_back(std::move(node)); return WrapDomNode(doc, raw);
    });
    document->props["createComment"] = MakeNativeFn([&doc](const std::vector<Value> &, bool &, std::string &) {
        auto node = std::make_unique<DomNode>(); node->type = DomNodeType::Text;
        DomNode *raw = node.get(); doc.detached_nodes.push_back(std::move(node)); return WrapDomNode(doc, raw);
    });
    // The document is an event target too: its listeners hang off the root
    // node, which every bubbling event reaches.
    for (const char *forwarded : {"addEventListener", "removeEventListener"}) {
        const std::string name = forwarded;
        document->props[name] = MakeNativeFn([&doc, name](const std::vector<Value> &args, bool &threw, std::string &error) {
            if (!doc.root) return Value::Undef();
            return WrapDomNode(doc, doc.root.get()).obj->props[name].obj->native(const_cast<std::vector<Value> &>(args), threw, error);
        });
    }
    document->props["dispatchEvent"] = MakeNativeFn([&doc, &interp](const std::vector<Value> &args, bool &threw, std::string &error) {
        if (!doc.root) return Value::Bool(true);
        return DispatchDomEvent(interp, &doc, doc.root.get(), args, threw, error);
    });
    document->props["readyState"] = Value::Str("loading");
    {
        // DOM interface objects: not constructible, but their prototypes
        // chain the way the platform's do and carry the accessors that
        // frameworks look up (React sets input values through
        // HTMLInputElement.prototype's `value` setter).
        auto state = GetDomEventState(doc);
        auto make_interface = [&](const char *name, const char *parent) {
            auto proto = std::make_shared<ObjectData>();
            if (parent) proto->prototype = state->interface_protos[parent];
            Value ctor = MakeNativeFn([name](const std::vector<Value> &, bool &threw, std::string &error) { threw = true; error = std::string("Illegal constructor: ") + name; return Value::Undef(); });
            ctor.obj->props["prototype"] = Value::Obj(proto);
            proto->props["constructor"] = ctor;
            proto->non_enumerable.insert("constructor");
            state->interface_protos[name] = proto;
            global->Define(name, ctor);
            return proto;
        };
        make_interface("EventTarget", nullptr);
        make_interface("Node", "EventTarget");
        make_interface("Element", "Node");
        make_interface("Text", "Node");
        make_interface("DocumentFragment", "Node");
        make_interface("Document", "Node");
        make_interface("HTMLElement", "Element");
        make_interface("SVGElement", "Element");
        for (const char *name : {"HTMLInputElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLFormElement", "HTMLButtonElement", "HTMLAnchorElement",
                                 "HTMLImageElement", "HTMLIFrameElement", "HTMLOptionElement", "HTMLCanvasElement"}) {
            ObjectPtr proto = make_interface(name, "HTMLElement");
            const std::string interface_name = name;
            if (interface_name != "HTMLInputElement" && interface_name != "HTMLSelectElement" && interface_name != "HTMLTextAreaElement" && interface_name != "HTMLOptionElement") continue;
            auto this_node = []() -> DomNode * { const Value self = NativeThis(); return self.type == VType::Object && self.obj ? self.obj->dom_node : nullptr; };
            proto->getters["value"] = MakeNativeFn([this_node](const std::vector<Value> &, bool &, std::string &) { DomNode *n = this_node(); if (!n) return Value::Undef(); return Value::Str(n->tag == "select" ? SelectValue(n) : n->tag == "option" ? OptionValue(n) : n->form_value); }).obj;
            proto->setters["value"] = MakeNativeFn([this_node](const std::vector<Value> &args, bool &, std::string &) { if (DomNode *n = this_node()) { const std::string text = args.empty() ? "" : ToDisplayString(args[0]); if (n->tag == "select") SetSelectValue(n, text); else if (n->tag == "option") n->attrs["value"] = text; else n->form_value = text; } return Value::Undef(); }).obj;
            if (interface_name != "HTMLInputElement") continue;
            proto->getters["checked"] = MakeNativeFn([this_node](const std::vector<Value> &, bool &, std::string &) { DomNode *n = this_node(); return Value::Bool(n && n->form_checked); }).obj;
            proto->setters["checked"] = MakeNativeFn([this_node](const std::vector<Value> &args, bool &, std::string &) { if (DomNode *n = this_node()) n->form_checked = !args.empty() && args[0].Truthy(); return Value::Undef(); }).obj;
        }
        document->prototype = state->interface_protos["Document"];
    }
    GetDomEventState(doc)->document_object = document;
    document->props["nodeType"] = Value::Num(9);
    global->Define("document", Value::Obj(document));

    // A plain, otherwise-inert object -- enough for the extremely common
    // "window.Foo = {...}" config-stashing pattern (MathJax's own
    // bootstrap script, among many others) to assign a property instead
    // of throwing ReferenceError, without pretending this engine has any
    // of the real BOM (setTimeout/location/etc. -- see js_engine.h's own
    // header on what's deliberately not implemented yet).
    auto window = std::make_shared<ObjectData>();
    window->is_window = true;
    window->props["getComputedStyle"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        auto result = std::make_shared<ObjectData>();
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->dom_node) return Value::Obj(result);
        const ComputedStyle &style = args[0].obj->dom_node->style;
        auto color_string = [](unsigned char r, unsigned char g, unsigned char b) {
            char value[8]; std::snprintf(value, sizeof(value), "#%02x%02x%02x", r, g, b); return std::string(value);
        };
        if (style.has_color) result->props["color"] = Value::Str(color_string(style.color_r, style.color_g, style.color_b));
        if (style.has_bg) result->props["backgroundColor"] = Value::Str(color_string(style.bg_r, style.bg_g, style.bg_b));
        result->props["display"] = Value::Str(style.display_none ? "none" : (style.block ? "block" : "inline"));
        result->props["fontWeight"] = Value::Str(style.bold ? "bold" : "normal");
        result->props["fontStyle"] = Value::Str(style.italic ? "italic" : "normal");
        return Value::Obj(result);
    });
    document->props["defaultView"] = Value::Obj(window);
    {
        DomNode *window_key = &GetDomEventState(doc)->window_node;
        window_key->tag = "#window";
        window->props["addEventListener"] = MakeNativeFn([&doc, window_key](const std::vector<Value> &args, bool &, std::string &) {
            if (args.size() < 2 || args[0].type != VType::String || args[1].type != VType::Object || !args[1].obj || !args[1].obj->is_function) return Value::Undef();
            bool capture = args.size() > 2 && args[2].type == VType::Boolean && args[2].boolean;
            auto &listeners = GetDomEventState(doc)->listeners[window_key][args[0].str];
            for (const DomEventListener &listener : listeners) if (listener.callback == args[1].obj && listener.capture == capture) return Value::Undef();
            listeners.push_back({args[1].obj, capture, false});
            return Value::Undef();
        });
        window->props["removeEventListener"] = MakeNativeFn([&doc, window_key](const std::vector<Value> &args, bool &, std::string &) {
            if (args.size() < 2 || args[0].type != VType::String || args[1].type != VType::Object) return Value::Undef();
            auto &listeners = GetDomEventState(doc)->listeners[window_key][args[0].str];
            listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [&](const DomEventListener &listener) { return listener.callback == args[1].obj; }), listeners.end());
            return Value::Undef();
        });
        window->props["dispatchEvent"] = MakeNativeFn([&doc, &interp, window_key](const std::vector<Value> &args, bool &threw, std::string &error) {
            return DispatchDomEvent(interp, &doc, window_key, args, threw, error);
        });
    }
    window->props["innerWidth"] = Value::Num(1024);
    window->props["innerHeight"] = Value::Num(768);
    window->props["devicePixelRatio"] = Value::Num(1);
    {
        auto navigator = std::make_shared<ObjectData>();
        navigator->props["userAgent"] = Value::Str("Mozilla/5.0 (mep) mep-html/1.0");
        navigator->props["language"] = Value::Str("en-US");
        navigator->props["platform"] = Value::Str("mep");
        navigator->props["onLine"] = Value::Bool(true);
        window->props["navigator"] = Value::Obj(navigator);
        auto performance = std::make_shared<ObjectData>();
        const auto origin = std::chrono::steady_clock::now();
        performance->props["now"] = MakeNativeFn([origin](const std::vector<Value> &, bool &, std::string &) {
            return Value::Num(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count());
        });
        window->props["performance"] = Value::Obj(performance);
    }
    auto make_storage = []() {
        auto values = std::make_shared<std::unordered_map<std::string, std::string>>();
        auto storage = std::make_shared<ObjectData>();
        storage->props["getItem"] = MakeNativeFn([values](const std::vector<Value> &args, bool &, std::string &) { if (args.empty()) return Value::MakeNull(); auto found = values->find(ToDisplayString(args[0])); return found == values->end() ? Value::MakeNull() : Value::Str(found->second); });
        storage->props["setItem"] = MakeNativeFn([values](const std::vector<Value> &args, bool &, std::string &) { if (args.size() >= 2) (*values)[ToDisplayString(args[0])] = ToDisplayString(args[1]); return Value::Undef(); });
        storage->props["removeItem"] = MakeNativeFn([values](const std::vector<Value> &args, bool &, std::string &) { if (!args.empty()) values->erase(ToDisplayString(args[0])); return Value::Undef(); });
        storage->props["clear"] = MakeNativeFn([values](const std::vector<Value> &, bool &, std::string &) { values->clear(); return Value::Undef(); });
        return storage;
    };
    auto local_storage = make_storage(), session_storage = make_storage();
    window->props["localStorage"] = Value::Obj(local_storage);
    window->props["sessionStorage"] = Value::Obj(session_storage);
    global->Define("localStorage", Value::Obj(local_storage));
    global->Define("sessionStorage", Value::Obj(session_storage));
    auto location = std::make_shared<ObjectData>();
    location->is_location = true;
    location->props["href"] = Value::Str(""); location->props["pathname"] = Value::Str(""); location->props["search"] = Value::Str(""); location->props["hash"] = Value::Str("");
    auto apply_location = [location](const Value &url) { SetProp(location, "href", url); };
    if (!doc.document_url.empty()) apply_location(Value::Str(doc.document_url));
    location->props["assign"] = MakeNativeFn([apply_location](const std::vector<Value> &args, bool &, std::string &) { if (!args.empty()) apply_location(args[0]); return Value::Undef(); });
    location->props["replace"] = MakeNativeFn([apply_location](const std::vector<Value> &args, bool &, std::string &) { if (!args.empty()) apply_location(args[0]); return Value::Undef(); });
    location->props["reload"] = MakeNativeFn([](const std::vector<Value> &, bool &, std::string &) { return Value::Undef(); });
    auto history = std::make_shared<ObjectData>();
    history->props["state"] = Value::MakeNull();
    history->props["length"] = Value::Num(1);
    auto entries = std::make_shared<std::vector<std::pair<Value, std::string>>>();
    entries->emplace_back(Value::MakeNull(), "");
    auto history_index = std::make_shared<size_t>(0);
    auto pop = [&interp, window, history, entries, history_index, apply_location](size_t index) {
        *history_index = index;
        history->props["state"] = (*entries)[index].first;
        apply_location(Value::Str((*entries)[index].second));
        auto event = std::make_shared<ObjectData>(); event->props["type"] = Value::Str("popstate"); event->props["state"] = (*entries)[index].first;
        Value callback = GetProp(window, "onpopstate");
        if (callback.type == VType::Object && callback.obj && callback.obj->is_function) {
            std::vector<Value> args{Value::Obj(event)}; (void)CallFunction(interp, callback.obj, args, nullptr);
        }
        if (HtmlDoc *active = g_active_doc) {
            std::vector<Value> args{Value::Obj(event)};
            bool threw = false; std::string error;
            (void)DispatchDomEvent(interp, active, &GetDomEventState(*active)->window_node, args, threw, error);
        }
    };
    history->props["pushState"] = MakeNativeFn([apply_location, history, entries, history_index, location](const std::vector<Value> &args, bool &, std::string &) {
        Value state = args.empty() ? Value::MakeNull() : args[0];
        std::string url = args.size() > 2 ? ToDisplayString(args[2]) : ToDisplayString(GetProp(location, "href"));
        entries->erase(entries->begin() + static_cast<std::ptrdiff_t>(*history_index + 1), entries->end());
        entries->emplace_back(state, url); *history_index = entries->size() - 1; history->props["state"] = state; history->props["length"] = Value::Num(static_cast<double>(entries->size())); apply_location(Value::Str(url)); return Value::Undef();
    });
    history->props["replaceState"] = MakeNativeFn([apply_location, history, entries, history_index](const std::vector<Value> &args, bool &, std::string &) {
        Value state = args.empty() ? Value::MakeNull() : args[0];
        std::string url = args.size() > 2 ? ToDisplayString(args[2]) : (*entries)[*history_index].second;
        (*entries)[*history_index] = {state, url}; history->props["state"] = state; apply_location(Value::Str(url)); return Value::Undef();
    });
    history->props["back"] = MakeNativeFn([pop, history_index](const std::vector<Value> &, bool &, std::string &) { if (*history_index > 0) pop(*history_index - 1); return Value::Undef(); });
    history->props["forward"] = MakeNativeFn([pop, history_index, entries](const std::vector<Value> &, bool &, std::string &) { if (*history_index + 1 < entries->size()) pop(*history_index + 1); return Value::Undef(); });
    history->props["go"] = MakeNativeFn([pop, history_index, entries](const std::vector<Value> &args, bool &, std::string &) { long offset = args.empty() ? 0 : static_cast<long>(ToNumber(args[0])); long target = static_cast<long>(*history_index) + offset; if (target >= 0 && static_cast<size_t>(target) < entries->size()) pop(static_cast<size_t>(target)); return Value::Undef(); });
    interp.window_object = window;
    window->props["location"] = Value::Obj(location);
    window->location_object = location;
    window->props["history"] = Value::Obj(history);
    global->Define("location", Value::Obj(location));
    global->Define("history", Value::Obj(history));
    global->Define("window", Value::Obj(window));
    window->props["__mepResolveUrl"] = MakeNativeFn([&doc](const std::vector<Value> &args, bool &, std::string &) {
        const std::string base = args.empty() ? "" : ToDisplayString(args[0]);
        const std::string ref = args.size() < 2 ? "" : ToDisplayString(args[1]);
        if (urlutil::HasScheme(ref)) return Value::Str(urlutil::ResolveUrl(ref, ref));
        return Value::Str(urlutil::ResolveUrl(base.empty() ? doc.document_url : urlutil::ResolveUrl(doc.document_url, base), ref));
    });
    global->Define("globalThis", Value::Obj(window));
    global->Define("self", Value::Obj(window));
    global->Define("this", Value::Obj(window));  // top-level `this`
}

}  // namespace

// Everything a page's scripts need to keep running after load: the
// interpreter (globals, timers, microtasks, suspended coroutines), the
// parsed programs its function objects point into, and the host callbacks.
// Heap-allocated and never moved: native closures hold references into it.
struct JsRuntime {
    HtmlDoc *doc = nullptr;
    Interpreter interp;
    std::vector<NodePtr> programs;
    std::vector<std::unique_ptr<std::string>> sources;  // what each program's nodes quote in errors
    struct Module {
        std::string url;
        const Node *program = nullptr;
        EnvPtr env;
        ObjectPtr namespace_object;
        int state = 0;  // 0 created, 1 linking/evaluating, 2 done, 3 failed
        Value failure;
        std::unordered_map<std::string, std::string> resolved;  // specifier -> absolute URL
    };
    std::unordered_map<std::string, std::unique_ptr<Module>> modules;
    std::function<void(const std::string &)> on_console_log;
    std::function<void(const std::string &)> on_error;
    bool dom_dirty = false;
};

namespace {

// Scopes g_active_interp/g_active_doc to one entry into script code.
struct ActiveRuntime {
    Interpreter *saved_interp = g_active_interp;
    HtmlDoc *saved_doc = g_active_doc;
    explicit ActiveRuntime(JsRuntime &runtime) { g_active_interp = &runtime.interp; g_active_doc = runtime.doc; }
    ~ActiveRuntime() { g_active_interp = saved_interp; g_active_doc = saved_doc; }
};

void ReportAbrupt(JsRuntime &runtime, const char *what, const Completion &completion) {
    if (completion.type == CompletionType::Throw) runtime.on_error(std::string(what) + ": " + ToDisplayString(runtime.interp.WrapThrown(completion.value)));
}

void FireLifecycleEvent(JsRuntime &runtime, DomNode *target, const std::string &type, bool bubbles) {
    if (!target) return;
    std::vector<Value> args{MakeSyntheticEvent(type, bubbles, false)};
    bool threw = false;
    std::string error;
    runtime.interp.steps = 0;
    (void)DispatchDomEvent(runtime.interp, runtime.doc, target, args, threw, error);
    if (threw) runtime.on_error("event error: " + error);
    ReportAbrupt(runtime, "microtask error", DrainMicrotasks(runtime.interp));
}

}  // namespace

namespace {

bool ReadModuleSource(const std::string &url, std::string &out, std::string &error) {
    if (urlutil::IsHttpUrl(url)) {
        const HtmlUrlFetcher &fetch = GetHtmlUrlFetcher();
        if (!fetch) { error = "no network access for " + url; return false; }
        HtmlFetchResult got = fetch(url);
        if (!got.error.empty() || got.status < 200 || got.status >= 300) { error = "Failed to fetch module " + url + (got.error.empty() ? " (" + std::to_string(got.status) + ")" : ": " + got.error); return false; }
        out = std::move(got.body);
        return true;
    }
    if (url.rfind("file://", 0) == 0) {
        std::ifstream in(url.substr(7), std::ios::binary);
        if (!in) { error = "Failed to read module " + url; return false; }
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }
    error = "unsupported module URL " + url;
    return false;
}

Completion LoadModule(JsRuntime &runtime, const std::string &url, const std::string *inline_source = nullptr);

// Where `name` exported by `module` really lives: the environment and local
// binding, following `export ... from` chains and `export *`.
bool ResolveModuleExport(JsRuntime &runtime, JsRuntime::Module &module, const std::string &name, std::pair<EnvPtr, std::string> &out, int depth = 0) {
    if (depth > 32) return false;
    for (const Node::ModuleExport &entry : module.program->module_exports) {
        if (entry.exported != name) continue;
        if (entry.from_specifier.empty()) { out = {module.env, entry.local}; return true; }
        auto dep = runtime.modules.find(module.resolved[entry.from_specifier]);
        if (dep == runtime.modules.end()) return false;
        if (entry.from_name == "*") { out = {module.env, "*ns:" + entry.exported + "*"}; return true; }
        return ResolveModuleExport(runtime, *dep->second, entry.from_name, out, depth + 1);
    }
    if (name == "default") return false;
    for (const Node::ModuleExport &entry : module.program->module_exports) {
        if (entry.exported != "*") continue;
        auto dep = runtime.modules.find(module.resolved[entry.from_specifier]);
        if (dep != runtime.modules.end() && ResolveModuleExport(runtime, *dep->second, name, out, depth + 1)) return true;
    }
    return false;
}

void CollectModuleExportNames(JsRuntime &runtime, JsRuntime::Module &module, std::vector<std::string> &names, int depth = 0) {
    if (depth > 32) return;
    for (const Node::ModuleExport &entry : module.program->module_exports) {
        if (entry.exported != "*") { if (std::find(names.begin(), names.end(), entry.exported) == names.end()) names.push_back(entry.exported); continue; }
        auto dep = runtime.modules.find(module.resolved[entry.from_specifier]);
        if (dep == runtime.modules.end()) continue;
        std::vector<std::string> inherited;
        CollectModuleExportNames(runtime, *dep->second, inherited, depth + 1);
        for (const std::string &name : inherited) if (name != "default" && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
}

// The module namespace object: one live accessor per export.
ObjectPtr ModuleNamespace(JsRuntime &runtime, JsRuntime::Module &module) {
    if (module.namespace_object) return module.namespace_object;
    auto ns = std::make_shared<ObjectData>();
    module.namespace_object = ns;
    std::vector<std::string> names;
    CollectModuleExportNames(runtime, module, names);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
        std::pair<EnvPtr, std::string> target;
        if (!ResolveModuleExport(runtime, module, name, target)) continue;
        ns->getters[name] = MakeNativeFn([target](const std::vector<Value> &, bool &threw, std::string &error) {
            Value *slot = target.first->FindOwn(target.second);
            if (!slot) { threw = true; error = "Cannot access '" + target.second + "' before initialization"; return Value::Undef(); }
            return *slot;
        }).obj;
    }
    ns->frozen = true;
    return ns;
}

// Fetches, parses, links and evaluates the module at `url` (once), its
// dependencies first. Returns its namespace object.
Completion LoadModule(JsRuntime &runtime, const std::string &url, const std::string *inline_source) {
    Interpreter &interp = runtime.interp;
    if (auto existing = runtime.modules.find(url); existing != runtime.modules.end()) {
        JsRuntime::Module &module = *existing->second;
        if (module.state == 3) return Completion::Thr(module.failure);
        return Completion::Norm(Value::Obj(ModuleNamespace(runtime, module)));  // also the answer inside an import cycle
    }
    auto owned = std::make_unique<JsRuntime::Module>();
    JsRuntime::Module &module = *owned;
    module.url = url;
    runtime.modules[url] = std::move(owned);
    auto fail = [&module](Value reason) { module.state = 3; module.failure = reason; return Completion::Thr(std::move(reason)); };

    std::string source;
    if (inline_source) source = *inline_source;
    else { std::string error; if (!ReadModuleSource(url, source, error)) return fail(Value::Str(error)); }
    runtime.sources.push_back(std::make_unique<std::string>(source));
    g_parse_source = runtime.sources.back().get();
    g_parse_pos = 0;
    Parser parser(source);
    parser.strict_mode = true;  // module code is always strict
    NodePtr program = parser.ParseProgram();
    g_parse_source = nullptr;
    if (!parser.ok) return fail(Value::Str("SyntaxError in " + url + ": " + parser.error));
    runtime.programs.push_back(std::move(program));
    module.program = runtime.programs.back().get();
    module.state = 1;
    module.env = std::make_shared<Environment>();
    module.env->parent = interp.global;
    module.env->is_function_scope = true;
    module.env->Define("this", Value::Undef());
    module.env->Define("*module-url*", Value::Str(url));

    // Dependencies first, in source order (imports and re-exports alike).
    std::vector<std::string> specifiers;
    for (const auto &entry : module.program->module_imports) if (std::find(specifiers.begin(), specifiers.end(), entry.specifier) == specifiers.end()) specifiers.push_back(entry.specifier);
    for (const auto &entry : module.program->module_exports) if (!entry.from_specifier.empty() && std::find(specifiers.begin(), specifiers.end(), entry.from_specifier) == specifiers.end()) specifiers.push_back(entry.from_specifier);
    for (const std::string &specifier : specifiers) {
        const bool relative = specifier.rfind("./", 0) == 0 || specifier.rfind("../", 0) == 0 || specifier.rfind("/", 0) == 0 || urlutil::HasScheme(specifier);
        if (!relative) return fail(Value::Str("Failed to resolve module specifier \"" + specifier + "\" (bare specifiers need an import map)"));
        module.resolved[specifier] = urlutil::ResolveUrl(url, specifier);
        Completion dependency = LoadModule(runtime, module.resolved[specifier]);
        if (dependency.type == CompletionType::Throw) return fail(dependency.value);
    }
    // Link: every import becomes an alias of the exporter's binding.
    for (const auto &entry : module.program->module_imports) {
        if (entry.imported.empty()) continue;
        JsRuntime::Module &dependency = *runtime.modules[module.resolved[entry.specifier]];
        if (entry.imported == "*") { module.env->Define(entry.local, Value::Obj(ModuleNamespace(runtime, dependency))); continue; }
        std::pair<EnvPtr, std::string> target;
        if (!ResolveModuleExport(runtime, dependency, entry.imported, target)) {
            return fail(Value::Str("SyntaxError: The requested module '" + entry.specifier + "' does not provide an export named '" + entry.imported + "'"));
        }
        module.env->aliases[entry.local] = target;
    }
    for (const auto &entry : module.program->module_exports) {
        if (entry.from_name == "*" && entry.exported != "*") module.env->Define("*ns:" + entry.exported + "*", Value::Obj(ModuleNamespace(runtime, *runtime.modules[module.resolved[entry.from_specifier]])));
    }
    interp.steps = 0;
    HoistVars(*module.program, module.program->body, *module.env);
    Completion ran = ExecBlockBody(interp, module.program->body, module.env);
    if (ran.type == CompletionType::Throw) return fail(ran.value);
    module.state = 2;
    return Completion::Norm(Value::Obj(ModuleNamespace(runtime, module)));
}

}  // namespace

std::shared_ptr<JsRuntime> StartScripts(HtmlDoc &doc, std::function<void(const std::string &)> on_console_log,
                                        std::function<void(const std::string &)> on_error) {
    auto runtime = std::make_shared<JsRuntime>();
    runtime->doc = &doc;
    runtime->on_console_log = std::move(on_console_log);
    runtime->on_error = std::move(on_error);
    Interpreter &interp = runtime->interp;
    interp.global = std::make_shared<Environment>();
    interp.global->is_function_scope = true;
    ActiveRuntime active(*runtime);
    SetupGlobals(interp, doc, runtime->on_console_log);
    {
        // The JavaScript half of the standard library (js_prelude.inc).
        static const char *const kPrelude =
#include "js_prelude.inc"
            ;
        Parser prelude_parser(kPrelude);
        NodePtr prelude = prelude_parser.ParseProgram();
        if (!prelude_parser.ok) runtime->on_error("prelude parse error: " + prelude_parser.error);
        else {
            runtime->programs.push_back(std::move(prelude));
            EnvPtr scope = interp.global;
            HoistVars(*runtime->programs.back(), runtime->programs.back()->body, *scope);
            ReportAbrupt(*runtime, "prelude error", ExecBlockBody(interp, runtime->programs.back()->body, scope));
        }
    }
    // All script tags in one document share the same global scope, and each
    // parsed program stays alive with the runtime: function objects point
    // into its AST and may be called long after the script finished.
    interp.document_url = doc.document_url;
    JsRuntime *raw_runtime = runtime.get();
    interp.load_module = [raw_runtime](const std::string &url) { return LoadModule(*raw_runtime, url); };
    for (size_t script_index = 0; script_index < doc.scripts.size(); ++script_index) {
        const std::string &script = doc.scripts[script_index];
        if (script_index < doc.script_info.size() && doc.script_info[script_index].is_module) continue;  // deferred: below
        runtime->sources.push_back(std::make_unique<std::string>(script));
        g_parse_source = runtime->sources.back().get();
        g_parse_pos = 0;
        Parser parser(script);
        NodePtr program = parser.ParseProgram();
        g_parse_source = nullptr;
        if (!parser.ok) {
            runtime->on_error("script parse error: " + parser.error);
            continue;
        }
        runtime->programs.push_back(std::move(program));
        EnvPtr scope = interp.global;
        interp.steps = 0;
        HoistVars(*runtime->programs.back(), runtime->programs.back()->body, *scope);
        ReportAbrupt(*runtime, "script error", ExecBlockBody(interp, runtime->programs.back()->body, scope));
        ReportAbrupt(*runtime, "microtask error", DrainMicrotasks(interp));
    }
    // Module scripts are deferred: they run once the classic ones have, in
    // document order, each in its own scope with its imports linked first.
    for (size_t script_index = 0, inline_count = 0; script_index < doc.scripts.size() && script_index < doc.script_info.size(); ++script_index) {
        const HtmlDoc::ScriptInfo &info = doc.script_info[script_index];
        if (!info.is_module) continue;
        const std::string url = info.url.empty() ? doc.document_url + "#inline-module-" + std::to_string(++inline_count) : info.url;
        ReportAbrupt(*runtime, "module error", LoadModule(*runtime, url, &doc.scripts[script_index]));
        ReportAbrupt(*runtime, "microtask error", DrainMicrotasks(interp));
    }
    // The document is parsed and its scripts have run: the two lifecycle
    // events pages hang their start-up code on.
    if (Value *document = interp.global->Find("document"); document && document->type == VType::Object && document->obj) document->obj->props["readyState"] = Value::Str("interactive");
    FireLifecycleEvent(*runtime, doc.root.get(), "DOMContentLoaded", true);
    if (Value *document = interp.global->Find("document"); document && document->type == VType::Object && document->obj) document->obj->props["readyState"] = Value::Str("complete");
    FireLifecycleEvent(*runtime, &GetDomEventState(doc)->window_node, "load", false);
    bool ran = false;
    ReportAbrupt(*runtime, "timer error", RunDueTimers(interp, &ran));
    // Attribute/class/tree mutations can affect inherited and selector based
    // styles. Layout reads ComputedStyle directly, so refresh it once after
    // the document's synchronous script sequence completes.
    ComputeStyles(doc);
    return runtime;
}

bool PumpScripts(JsRuntime &runtime) {
    ActiveRuntime active(runtime);
    bool ran = !runtime.interp.microtasks.empty();
    ReportAbrupt(runtime, "microtask error", DrainMicrotasks(runtime.interp));
    ReportAbrupt(runtime, "timer error", RunDueTimers(runtime.interp, &ran));
    if (ran) ComputeStyles(*runtime.doc);
    return ran;
}

double ScriptsNextWakeMs(const JsRuntime &runtime) {
    if (!runtime.interp.microtasks.empty()) return 0;
    double soonest = -1;
    const auto now = std::chrono::steady_clock::now();
    for (const Interpreter::Timer &timer : runtime.interp.timers) {
        if (timer.canceled) continue;
        const double wait = std::max(0.0, std::chrono::duration<double, std::milli>(timer.deadline - now).count());
        if (soonest < 0 || wait < soonest) soonest = wait;
    }
    return soonest;
}

bool ScriptsClick(JsRuntime &runtime, DomNode *node) {
    if (!node) return false;
    ActiveRuntime active(runtime);
    bool threw = false;
    std::string error;
    runtime.interp.steps = 0;
    const bool proceed = ActivateDomNode(*runtime.doc, node, threw, error);
    if (threw) runtime.on_error("event error: " + error);
    ReportAbrupt(runtime, "microtask error", DrainMicrotasks(runtime.interp));
    ComputeStyles(*runtime.doc);
    return proceed && !threw;
}

bool ScriptsDispatchEvent(JsRuntime &runtime, DomNode *node, const std::string &type, bool bubbles) {
    if (!node) return true;
    ActiveRuntime active(runtime);
    bool threw = false;
    std::string error;
    runtime.interp.steps = 0;
    const bool proceed = FireDomEvent(*runtime.doc, node, type, bubbles, threw, error);
    if (threw) runtime.on_error("event error: " + error);
    ReportAbrupt(runtime, "microtask error", DrainMicrotasks(runtime.interp));
    ComputeStyles(*runtime.doc);
    return proceed;
}

bool ScriptsHaveListeners(JsRuntime &runtime) {
    auto state = GetDomEventState(*runtime.doc);
    return !state->listeners.empty() || !state->property_handlers.empty();
}

void RunScripts(HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log,
                 const std::function<void(const std::string &)> &on_error) {
    // One-shot form: run to the end of load and let the runtime go. Nodes
    // keep no pointers into it (listeners live in doc.js_event_state, which
    // the caller should treat as dead once this returns).
    std::shared_ptr<JsRuntime> runtime = StartScripts(doc, on_console_log, on_error);
    doc.js_event_state.reset();
}
