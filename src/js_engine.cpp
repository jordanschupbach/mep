#include "js_engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

struct ObjectData {
    std::unordered_map<std::string, Value> props;
    std::unordered_map<std::string, ObjectPtr> getters;
    std::unordered_map<std::string, ObjectPtr> setters;
    std::vector<std::string> private_field_names;
    ObjectPtr prototype;
    bool is_array = false;
    bool is_function = false;
    bool is_generator_function = false;
    bool is_async_function = false;
    bool is_regexp = false;
    bool is_promise = false;
    // 0 pending, 1 fulfilled, 2 rejected. Reactions retain their paired
    // fulfillment/rejection handlers and downstream promise.
    int promise_state = 0;
    Value promise_value;
    std::vector<std::tuple<ObjectPtr, ObjectPtr, ObjectPtr>> promise_reactions;
    std::shared_ptr<mep_regex::Regex> regexp;
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

    /**
     * @brief Looks up a binding by name, walking outward through parent scopes.
     * @param name The identifier to look up.
     * @return A pointer to the binding's Value if found in this scope or an ancestor, else nullptr.
     */
    Value *Find(const std::string &name) {
        for (Environment *e = this; e != nullptr; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
        }
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
    if (d == static_cast<double>(static_cast<long long>(d)) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        return buf;
    }
    std::ostringstream oss;
    oss.precision(15);
    oss << d;
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
    for (const auto &[name, value] : node->attrs) html += " " + name + "=\"" + EscapeHtmlText(value) + "\"";
    static const std::unordered_set<std::string> void_tags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    if (void_tags.count(node->tag)) return html + ">";
    html += ">"; for (const auto &child : node->children) html += SerializeDomNode(child.get()); return html + "</" + node->tag + ">";
}

void ReplaceInnerHtml(DomNode *node, const std::string &html) {
    if (!node || node->type != DomNodeType::Element) return;
    HtmlDoc fragment; ParseHtml(html, fragment);
    node->children.clear();
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
    n->children.clear();
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
Value GetProp(const ObjectPtr &obj, const std::string &key) {
    if (!obj) return Value::Undef();
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
        if (key == "shadowRoot") return node->shadow_root ? WrapDomNode(*obj->owner_doc, node->shadow_root.get()) : Value::MakeNull();
        if (key == "tagName" || key == "nodeName") return Value::Str(node->tag);
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
        const std::string href = ToDisplayString(val);
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
};

struct Token {
    Tok type = Tok::End;
    std::string text;  // identifier name, string literal's decoded value, or raw text for TemplateStr (re-scanned by the parser -- see ParseTemplateLiteral)
    double num = 0;
    int pos = 0;  // byte offset in source, for error messages
};

struct Lexer {
    std::string src;
    size_t i = 0;

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
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) i++;
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
        SkipTrivia();
        Token t;
        t.pos = static_cast<int>(i);
        if (i >= src.size()) {
            t.type = Tok::End;
            return t;
        }
        char c = src[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t start = i;
            while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) i++;
            if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                i++;
                if (i < src.size() && (src[i] == '+' || src[i] == '-')) i++;
                while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) i++;
            }
            t.type = Tok::Num;
            t.num = std::strtod(src.substr(start, i - start).c_str(), nullptr);
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
                {"of", Tok::KwOf},         {"in", Tok::KwIn},
                {"switch", Tok::KwSwitch}, {"case", Tok::KwCase},     {"default", Tok::KwDefault},
                {"true", Tok::KwTrue},     {"false", Tok::KwFalse},     {"null", Tok::KwNull},
                {"undefined", Tok::KwUndefined}, {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
                {"try", Tok::KwTry}, {"catch", Tok::KwCatch}, {"finally", Tok::KwFinally}, {"throw", Tok::KwThrow},
                {"async", Tok::KwAsync}, {"await", Tok::KwAwait},
                {"typeof", Tok::KwTypeof},
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
                if (i + 1 < src.size() && src[i + 1] == '.') {
                    i += 2;
                    t.type = Tok::QuestionDot;
                } else if (i + 1 < src.size() && src[i + 1] == '?') {
                    i += 2;
                    t.type = Tok::Nullish;
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
                two('=', Tok::StarEq, Tok::Star);
                return t;
            case '%':
                i++;
                t.type = Tok::Percent;
                return t;
            case '/':
                two('=', Tok::SlashEq, Tok::Slash);
                return t;
            case '<':
                two('=', Tok::LtEq, Tok::Lt);
                return t;
            case '>':
                two('=', Tok::GtEq, Tok::Gt);
                return t;
            case '&':
                if (i + 1 < src.size() && src[i + 1] == '&') {
                    i += 2;
                    t.type = Tok::AndAnd;
                    return t;
                }
                i++;
                t.type = Tok::End;  // unsupported bitwise &, treat as end-of-useful-input rather than mis-lex
                return t;
            case '|':
                if (i + 1 < src.size() && src[i + 1] == '|') {
                    i += 2;
                    t.type = Tok::OrOr;
                    return t;
                }
                i++;
                t.type = Tok::End;
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
};

struct Node {
    NodeKind kind;
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
    void Advance() { cur = lex.Next(); }

    /**
     * @brief Records a parse failure, keeping only the first one encountered.
     * @param msg The error message to record.
     */
    void Fail(const std::string &msg) {
        if (ok) {
            ok = false;
            error = msg;
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
        while (ok && !Check(Tok::End)) {
            prog->body.push_back(ParseStatement());
            if (!ok) break;
        }
        return prog;
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
        if (Check(Tok::KwAsync)) {
            Advance();
            if (!Check(Tok::KwFunction)) { Fail("async must precede function"); return std::make_unique<Node>(NodeKind::Block); }
            NodePtr function = ParseFunctionDecl();
            function->is_async = true;
            return function;
        }
        if (Check(Tok::KwClass)) return ParseClassDecl();
        if (Check(Tok::KwIf)) return ParseIf();
        if (Check(Tok::KwWhile)) return ParseWhile();
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
        if (Check(Tok::KwYield)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Yield);
            if (!Check(Tok::Semicolon) && !Check(Tok::RBrace) && !Check(Tok::End)) n->a = ParseExpression();
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwReturn)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Return);
            if (!Check(Tok::Semicolon) && !Check(Tok::RBrace) && !Check(Tok::End)) n->a = ParseExpression();
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwBreak)) {
            Advance();
            std::string label;
            if (Check(Tok::Ident)) { label = cur.text; Advance(); }
            Match(Tok::Semicolon);
            auto n = std::make_unique<Node>(NodeKind::Break);
            n->name = std::move(label);
            return n;
        }
        if (Check(Tok::KwContinue)) {
            Advance();
            std::string label;
            if (Check(Tok::Ident)) { label = cur.text; Advance(); }
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
            if (next.type == Tok::Colon) {
                Advance();
                Expect(Tok::Colon, "':'");
                NodePtr target = ParseStatement();
                if (target->kind == NodeKind::For || target->kind == NodeKind::While) {
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
            Expect(Tok::LParen, "'(' after catch");
            if (!Check(Tok::Ident)) Fail("expected catch binding");
            else { n->name = cur.text; Advance(); }
            Expect(Tok::RParen, "')' after catch binding");
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
                if (!Check(Tok::Ident) && !Check(Tok::Str)) {
                    Fail("expected property key in binding pattern");
                    break;
                }
                std::string key = cur.text;
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
        Advance();  // var/let/const
        auto n = std::make_unique<Node>(NodeKind::VarDecl);
        for (;;) {
            if (Check(Tok::LBracket) || Check(Tok::LBrace)) {
                auto pattern = ParseBindingPattern();
                NodePtr init;
                if (Match(Tok::Assign)) init = ParseAssignExpr();
                n->pattern_declarators.emplace_back(std::move(pattern), std::move(init));
            } else if (Check(Tok::Ident)) {
                std::string name = cur.text;
                Advance();
                NodePtr init;
                if (Match(Tok::Assign)) init = ParseAssignExpr();
                n->declarators.emplace_back(name, std::move(init));
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
        if (!Check(Tok::Ident)) {
            Fail("expected class name");
            return n;
        }
        n->name = cur.text;
        Advance();
        if (Match(Tok::KwExtends)) n->a = ParseCallOrMember();
        Expect(Tok::LBrace, "'{'");
        while (ok && !Check(Tok::RBrace)) {
            if (Check(Tok::PrivateIdent)) {
                n->class_private_fields.push_back(cur.text);
                Advance();
                if (Match(Tok::Assign)) n->class_private_initializers.push_back(ParseAssignExpr());
                else n->class_private_initializers.push_back(nullptr);
                Match(Tok::Semicolon);
                continue;
            }
            bool is_static = false;
            if (Check(Tok::Ident) && cur.text == "static") { is_static = true; Advance(); }
            int accessor = 0;
            if (Check(Tok::Ident) && (cur.text == "get" || cur.text == "set")) {
                accessor = cur.text == "get" ? 1 : 2;
                Advance();
            }
            if (!Check(Tok::Ident)) { Fail("expected method name"); break; }
            std::string method_name = cur.text;
            Advance();
            if (is_static && Match(Tok::Assign)) {
                n->class_static_fields.emplace_back(std::move(method_name), ParseAssignExpr());
                Match(Tok::Semicolon);
                continue;
            }
            if (is_static && Match(Tok::Semicolon)) {
                n->class_static_fields.emplace_back(std::move(method_name), nullptr);
                continue;
            }
            auto method = std::make_unique<Node>(NodeKind::FunctionExpr);
            method->name = method_name;
            ParseParamsAndBody(*method);
            n->class_method_names.push_back(std::move(method_name));
            n->class_method_static.push_back(is_static);
            n->class_method_accessor.push_back(accessor);
            n->body.push_back(std::move(method));
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
        NodePtr blk = ParseBlock();
        n.body = std::move(blk->body);
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
            Advance();
            if (!Check(Tok::Ident)) {
                Fail("expected identifier in for declaration");
                return n;
            }
            const std::string first_name = cur.text;
            Advance();
            if (Check(Tok::KwOf) || Check(Tok::KwIn)) {
                n->name = first_name;
                n->boolean = true;  // declaration form
                n->op = Check(Tok::KwOf) ? "of" : "in";
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
            NodePtr init;
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
            n->init = std::move(decl);
            Expect(Tok::Semicolon, "';'");
        } else if (Check(Tok::Ident)) {
            const size_t save_i = lex.i;
            const Token save_cur = cur;
            const std::string name = cur.text;
            Advance();
            if (Check(Tok::KwOf) || Check(Tok::KwIn)) {
                n->name = name;
                n->boolean = false;  // assign to an existing binding
                n->op = Check(Tok::KwOf) ? "of" : "in";
                Advance();
                n->a = ParseExpression();
                Expect(Tok::RParen, "')'");
                n->then_branch = ParseStatement();
                return n;
            }
            lex.i = save_i;
            cur = save_cur;
            auto es = std::make_unique<Node>(NodeKind::ExprStmt);
            es->a = ParseExpression();
            n->init = std::move(es);
            Expect(Tok::Semicolon, "';'");
        } else if (!Check(Tok::Semicolon)) {
            auto es = std::make_unique<Node>(NodeKind::ExprStmt);
            es->a = ParseExpression();
            n->init = std::move(es);
            Expect(Tok::Semicolon, "';'");
        } else {
            Advance();
        }
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
    NodePtr ParseExpression() { return ParseAssignExpr(); }

    /**
     * @brief Tests whether a token type is one of the supported assignment operators (=, +=, -=, *=, /=).
     * @param t The token type to test.
     * @return true if t is an assignment operator, false otherwise.
     */
    bool IsAssignOp(Tok t) const {
        return t == Tok::Assign || t == Tok::PlusEq || t == Tok::MinusEq || t == Tok::StarEq || t == Tok::SlashEq;
    }

    /**
     * @brief Parses an assignment expression: first speculatively tries an arrow-function form (`ident =>` or `(params) =>`, rewinding the lexer if it doesn't pan out), otherwise parses a conditional expression and, if an assignment operator follows, wraps it as an Assign node.
     * @return The parsed expression node (a FunctionExpr for an arrow function, an Assign node for an assignment, or whatever ParseConditional produced otherwise).
     */
    NodePtr ParseAssignExpr() {
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
            ok = save_ok;
            error.clear();
        }

        NodePtr left = ParseConditional();
        if (ok && IsAssignOp(cur.type)) {
            std::string op = cur.text;
            Tok t = cur.type;
            op = (t == Tok::Assign) ? "=" : (t == Tok::PlusEq) ? "+=" : (t == Tok::MinusEq) ? "-=" : (t == Tok::StarEq) ? "*=" : "/=";
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
        NodePtr left = ParseEquality();
        while (Check(Tok::AndAnd)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "&&";
            n->a = std::move(left);
            n->b = ParseEquality();
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
            if (Check(Tok::EqEq) || Check(Tok::EqEqEq)) op = "==";
            else if (Check(Tok::NotEq) || Check(Tok::NotEqEq))
                op = "!=";
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
        NodePtr left = ParseAdditive();
        for (;;) {
            std::string op;
            if (Check(Tok::Lt)) op = "<";
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
        NodePtr left = ParseUnary();
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
            n->b = ParseUnary();
            left = std::move(n);
        }
        return left;
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
        if (Check(Tok::Minus) || Check(Tok::Plus) || Check(Tok::Bang) || Check(Tok::KwTypeof)) {
            std::string op = Check(Tok::Minus) ? "-" : Check(Tok::Plus) ? "+" : Check(Tok::Bang) ? "!" : "typeof";
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
        if (Check(Tok::PlusPlus) || Check(Tok::MinusMinus)) {
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
            construct->a = ParsePrimary();
            if (Match(Tok::LParen)) {
                while (ok && !Check(Tok::RParen)) {
                    construct->args.push_back(ParseAssignExpr());
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
                if (!Check(Tok::Ident) && !Check(Tok::PrivateIdent) && !Check(Tok::KwOf) && !Check(Tok::KwIn) && !Check(Tok::KwCatch) && !Check(Tok::KwFinally)) {
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
                } else if (Check(Tok::Ident) || Check(Tok::KwOf) || Check(Tok::KwIn)) {
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
        if (Check(Tok::Ident)) {
            auto n = std::make_unique<Node>(NodeKind::Ident);
            n->name = cur.text;
            Advance();
            return n;
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
            while (ok && !Check(Tok::RBrace)) {
                std::string key;
                bool shorthand = false;
                bool is_computed = false;
                bool is_spread = false;
                NodePtr computed_key;
                if (Match(Tok::Ellipsis)) {
                    is_spread = true;
                    n->obj_props.emplace_back("", ParseAssignExpr());
                    n->obj_prop_key_exprs.push_back(nullptr);
                    n->obj_prop_spread.push_back(true);
                } else if (Match(Tok::LBracket)) {
                    is_computed = true;
                    computed_key = ParseExpression();
                    Expect(Tok::RBracket, "']'");
                    Expect(Tok::Colon, "':'");
                    n->obj_props.emplace_back("", ParseAssignExpr());
                    n->obj_prop_key_exprs.push_back(std::move(computed_key));
                    n->obj_prop_spread.push_back(false);
                } else if (Check(Tok::Ident) || Check(Tok::KwTrue) || Check(Tok::KwFalse) || Check(Tok::KwNull)) {
                    shorthand = Check(Tok::Ident);
                    key = cur.text.empty() ? "" : cur.text;
                    Advance();
                } else if (Check(Tok::Str)) {
                    key = cur.text;
                    Advance();
                } else {
                    Fail("expected property key");
                    break;
                }
                if (is_spread || is_computed) {
                    // The computed-key branch has already consumed its
                    // value and appended the property above.
                } else if (Match(Tok::Colon)) {
                    n->obj_props.emplace_back(key, ParseAssignExpr());
                    n->obj_prop_key_exprs.push_back(nullptr);
                    n->obj_prop_spread.push_back(false);
                } else if (Check(Tok::LParen)) {
                    auto method = std::make_unique<Node>(NodeKind::FunctionExpr);
                    method->name = key;
                    ParseParamsAndBody(*method);
                    n->obj_props.emplace_back(key, std::move(method));
                    n->obj_prop_key_exprs.push_back(nullptr);
                    n->obj_prop_spread.push_back(false);
                } else if (shorthand) {
                    auto value = std::make_unique<Node>(NodeKind::Ident);
                    value->name = key;
                    n->obj_props.emplace_back(key, std::move(value));
                    n->obj_prop_key_exprs.push_back(nullptr);
                    n->obj_prop_spread.push_back(false);
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
constexpr long kMaxSteps = 4'000'000;
constexpr int kMaxCallDepth = 300;

struct Interpreter {
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
        Value v = fn->native(args, threw, err);
        return threw ? Completion::Thr(err) : Completion::Norm(v);
    }
    if (!fn->fn_node) return Completion::Thr("value is not callable");
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
    scope->Define("this", this_value ? *this_value : Value::Undef());
    if (fn->super_class) {
        ObjectPtr base_class = fn->super_class;
        Value super_value = MakeNativeFn([&interp, scope, base_class](std::vector<Value> &super_args, bool &threw, std::string &err) {
            Value base_constructor = GetProp(base_class, "constructor");
            Value *receiver = scope->Find("this");
            if (base_constructor.type != VType::Object || !base_constructor.obj || !base_constructor.obj->is_function || !receiver) {
                threw = true;
                err = "super constructor is unavailable";
                return Value::Undef();
            }
            Completion called = CallFunction(interp, base_constructor.obj, super_args, receiver);
            if (called.IsAbrupt()) { threw = true; err = called.label; return Value::Undef(); }
            return called.value;
        });
        // `super.method()` is a method lookup on a receiver-bound facade,
        // while bare `super(...)` remains the base constructor call above.
        for (ObjectPtr proto = base_class->prototype; proto; proto = proto->prototype) {
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

Completion RunDueTimers(Interpreter &interp) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t index = 0; index < interp.timers.size();) {
        Interpreter::Timer &timer = interp.timers[index];
        if (timer.canceled) { interp.timers.erase(interp.timers.begin() + static_cast<std::ptrdiff_t>(index)); continue; }
        if (timer.deadline > now) { ++index; continue; }
        std::vector<Value> args = timer.args;
        if (timer.animation_frame) {
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            args.push_back(Value::Num(static_cast<double>(milliseconds)));
        }
        Completion result = CallFunction(interp, timer.callback, args);
        if (!timer.repeat || timer.canceled) interp.timers.erase(interp.timers.begin() + static_cast<std::ptrdiff_t>(index));
        else { timer.deadline = now + std::max(timer.interval, std::chrono::milliseconds(1)); ++index; }
        if (result.IsAbrupt()) return result;
        Completion microtasks = DrainMicrotasks(interp);
        if (microtasks.IsAbrupt()) return microtasks;
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
            out = Completion::Thr("cannot set property of non-object");
            return false;
        }
        std::string key = target.prop_name;
            if (target.computed) {
            Completion keyc = EvalExpr(interp, *target.b, env);
            if (keyc.IsAbrupt()) {
                out = keyc;
                return false;
            }
                key = keyc.value.type == VType::Number ? NumberToString(keyc.value.num) : ToDisplayString(keyc.value);
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
                    if (c.value.type != VType::Object || !c.value.obj || !c.value.obj->is_array)
                        return Completion::Thr("array spread requires an array");
                    const long length = ArrayLength(c.value.obj);
                    for (long j = 0; j < length; ++j) obj->props[std::to_string(out_index++)] = GetProp(c.value.obj, std::to_string(j));
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
                    for (const auto &[spread_key, spread_value] : source.value.obj->props) {
                        if (!(source.value.obj->is_array && spread_key == "length")) obj->props[spread_key] = spread_value;
                    }
                    continue;
                }
                std::string key = kv.first;
                if (i < n.obj_prop_key_exprs.size() && n.obj_prop_key_exprs[i]) {
                    Completion key_completion = EvalExpr(interp, *n.obj_prop_key_exprs[i], env);
                    if (key_completion.IsAbrupt()) return key_completion;
                    key = key_completion.value.type == VType::Number ? NumberToString(key_completion.value.num)
                                                                      : ToDisplayString(key_completion.value);
                }
                Completion c = EvalExpr(interp, *kv.second, env);
                if (c.IsAbrupt()) return c;
                obj->props[key] = c.value;
            }
            return Completion::Norm(Value::Obj(obj));
        }
        case NodeKind::Ident: {
            const Value *slot = env->Find(n.name);
            if (!slot) return Completion::Thr("'" + n.name + "' is not defined");
            return Completion::Norm(*slot);
        }
        case NodeKind::Unary: {
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
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
                        return Completion::Norm(Value::Str(c.value.obj && c.value.obj->is_function ? "function" : "object"));
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
            const Value &lv = l.value;
            const Value &rv = r.value;
            if (n.op == "+") {
                if (lv.type == VType::String || rv.type == VType::String) {
                    return Completion::Norm(Value::Str(ToDisplayString(lv) + ToDisplayString(rv)));
                }
                return Completion::Norm(Value::Num(ToNumber(lv) + ToNumber(rv)));
            }
            if (n.op == "-") return Completion::Norm(Value::Num(ToNumber(lv) - ToNumber(rv)));
            if (n.op == "*") return Completion::Norm(Value::Num(ToNumber(lv) * ToNumber(rv)));
            if (n.op == "/") return Completion::Norm(Value::Num(ToNumber(lv) / ToNumber(rv)));
            if (n.op == "%") return Completion::Norm(Value::Num(std::fmod(ToNumber(lv), ToNumber(rv))));
            if (n.op == "==") return Completion::Norm(Value::Bool(StrictEquals(lv, rv)));
            if (n.op == "!=") return Completion::Norm(Value::Bool(!StrictEquals(lv, rv)));
            if (n.op == "<" || n.op == ">" || n.op == "<=" || n.op == ">=") {
                bool result;
                if (lv.type == VType::String && rv.type == VType::String) {
                    result = n.op == "<" ? lv.str < rv.str : n.op == ">" ? lv.str > rv.str : n.op == "<=" ? lv.str <= rv.str : lv.str >= rv.str;
                } else {
                    double a = ToNumber(lv), b = ToNumber(rv);
                    result = n.op == "<" ? a < b : n.op == ">" ? a > b : n.op == "<=" ? a <= b : a >= b;
                }
                return Completion::Norm(Value::Bool(result));
            }
            return Completion::Thr("unsupported binary operator '" + n.op + "'");
        }
        case NodeKind::Assign: {
            Completion rhs;
            if (n.op == "=") {
                rhs = EvalExpr(interp, *n.b, env);
            } else {
                Completion cur = EvalExpr(interp, *n.a, env);
                if (cur.IsAbrupt()) return cur;
                Completion r = EvalExpr(interp, *n.b, env);
                if (r.IsAbrupt()) return r;
                if (n.op == "+=" && (cur.value.type == VType::String || r.value.type == VType::String)) {
                    rhs = Completion::Norm(Value::Str(ToDisplayString(cur.value) + ToDisplayString(r.value)));
                } else {
                    double a = ToNumber(cur.value), b = ToNumber(r.value);
                    double result = n.op == "+=" ? a + b : n.op == "-=" ? a - b : n.op == "*=" ? a * b : a / b;
                    rhs = Completion::Norm(Value::Num(result));
                }
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
            if (objc.value.type == VType::Number) {
                std::string key = n.prop_name;
                if (n.computed) {
                    Completion keyc = EvalExpr(interp, *n.b, env);
                    if (keyc.IsAbrupt()) return keyc;
                    key = ToDisplayString(keyc.value);
                }
                const double source = objc.value.num;
                if (key == "toString") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &, bool &, std::string &) { return Value::Str(NumberToString(source)); }));
                if (key == "toFixed") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { int digits = args.empty() ? 0 : std::max(0, std::min(100, static_cast<int>(ToNumber(args[0])))); std::ostringstream out; out << std::fixed << std::setprecision(digits) << source; return Value::Str(out.str()); }));
                if (key == "toPrecision") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { if (args.empty()) return Value::Str(NumberToString(source)); int digits = std::max(1, std::min(100, static_cast<int>(ToNumber(args[0])))); std::ostringstream out; out << std::setprecision(digits) << source; return Value::Str(out.str()); }));
                return Completion::Norm(Value::Undef());
            }
            if (objc.value.type == VType::String) {
                std::string key = n.prop_name;
                if (n.computed) {
                    Completion keyc = EvalExpr(interp, *n.b, env);
                    if (keyc.IsAbrupt()) return keyc;
                    key = ToDisplayString(keyc.value);
                }
                const std::string source = objc.value.str;
                if (key == "length") return Completion::Norm(Value::Num(static_cast<double>(source.size())));
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
                if (key == "replace" || key == "replaceAll") return Completion::Norm(MakeNativeFn([source, key](const std::vector<Value> &args, bool &, std::string &) { if (args.size() < 2) return Value::Str(source); if (args[0].type == VType::Object && args[0].obj && args[0].obj->is_regexp && args[0].obj->regexp) return Value::Str(key == "replaceAll" ? args[0].obj->regexp->ReplaceAll(source, ToDisplayString(args[1])) : args[0].obj->regexp->ReplaceFirst(source, ToDisplayString(args[1]))); std::string needle = ToDisplayString(args[0]), replacement = ToDisplayString(args[1]), out = source; if (needle.empty()) return Value::Str(out); size_t at = out.find(needle); while (at != std::string::npos) { out.replace(at, needle.size(), replacement); if (key == "replace") break; at = out.find(needle, at + replacement.size()); } return Value::Str(out); }));
                if (key == "match") return Completion::Norm(MakeNativeFn([source](const std::vector<Value> &args, bool &, std::string &) { if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->is_regexp || !args[0].obj->regexp) return Value::MakeNull(); mep_regex::Match found = args[0].obj->regexp->Search(source); if (!found.ok()) return Value::MakeNull(); auto result = std::make_shared<ObjectData>(); result->is_array = true; for (size_t i = 0; i < found.groups.size(); ++i) { const auto group = found.groups[i]; result->props[std::to_string(i)] = group.first < 0 ? Value::Undef() : Value::Str(source.substr(static_cast<size_t>(group.first), static_cast<size_t>(group.second - group.first))); } result->props["length"] = Value::Num(static_cast<double>(found.groups.size())); return Value::Obj(result); }));
                return Completion::Norm(Value::Undef());
            }
            if (objc.value.type != VType::Object || !objc.value.obj) {
                if (n.optional && (objc.value.type == VType::Null || objc.value.type == VType::Undefined)) {
                    return Completion::Norm(Value::Undef());
                }
                return Completion::Thr("cannot read property of " + ToDisplayString(objc.value));
            }
            std::string key = n.prop_name;
            if (n.computed) {
                Completion keyc = EvalExpr(interp, *n.b, env);
                if (keyc.IsAbrupt()) return keyc;
                key = keyc.value.type == VType::Number ? NumberToString(keyc.value.num) : ToDisplayString(keyc.value);
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
                    if (args.empty() || args[0].type != VType::Object || !args[0].obj) { threw = true; error = "dispatchEvent requires an Event"; return Value::Undef(); }
                    ObjectPtr event = args[0].obj;
                    Value type = GetProp(event, "type");
                    if (type.type != VType::String || type.str.empty()) { threw = true; error = "event type is required"; return Value::Undef(); }
                    event->props["target"] = WrapDomNode(*doc, target);
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
                    for (DomNode *node = target; node; node = node->parent) path.push_back(node);
                    auto state = GetDomEventState(*doc);
                    auto fire = [&](DomNode *node, bool capture) -> bool {
                        event->props["currentTarget"] = WrapDomNode(*doc, node);
                        auto node_listeners = state->listeners.find(node);
                        if (node_listeners != state->listeners.end()) {
                            auto typed = node_listeners->second.find(type.str);
                            if (typed != node_listeners->second.end()) for (size_t index = 0; index < typed->second.size();) {
                                DomEventListener listener = typed->second[index];
                                if (listener.capture != capture) { ++index; continue; }
                                std::vector<Value> event_args{Value::Obj(event)};
                                Value receiver = WrapDomNode(*doc, node);
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
                                    Value receiver = WrapDomNode(*doc, node);
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
            Value ordinary = GetProp(objc.value.obj, key);
            if (ordinary.type != VType::Undefined) {
                // Bind ordinary user methods here so a Call node never has
                // to evaluate a side-effecting member base a second time
                // merely to recover `this`.
                if (ordinary.type == VType::Object && ordinary.obj && ordinary.obj->is_function && !ordinary.obj->native) {
                    ObjectPtr method = ordinary.obj;
                    Value receiver = objc.value;
                    return Completion::Norm(MakeNativeFn([&interp, method, receiver](std::vector<Value> &args, bool &threw, std::string &error) mutable {
                        Completion called = CallFunction(interp, method, args, &receiver);
                        if (called.IsAbrupt()) { threw = true; error = ToDisplayString(called.value); return Value::Undef(); }
                        return called.value;
                    }));
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
            if (key == "hasOwnProperty") {
                ObjectPtr target = objc.value.obj;
                return Completion::Norm(MakeNativeFn([target](const std::vector<Value> &args, bool &, std::string &) {
                    return Value::Bool(!args.empty() && target->props.contains(ToDisplayString(args[0])));
                }));
            }
            if (key == "valueOf") {
                Value target = objc.value;
                return Completion::Norm(MakeNativeFn([target](const std::vector<Value> &, bool &, std::string &) { return target; }));
            }
            if (key == "toString") {
                const std::string tag = objc.value.obj->is_array ? "[object Array]" : "[object Object]";
                return Completion::Norm(MakeNativeFn([tag](const std::vector<Value> &, bool &, std::string &) { return Value::Str(tag); }));
            }
            return Completion::Norm(ordinary);
        }
        case NodeKind::Call: {
            Completion calleec = EvalExpr(interp, *n.a, env);
            if (calleec.IsAbrupt()) return calleec;
            if (calleec.value.type != VType::Object || !calleec.value.obj || !calleec.value.obj->is_function) {
                const bool chained_optional_member = n.a && n.a->kind == NodeKind::Member && n.a->optional;
                if ((n.optional || chained_optional_member) &&
                    (calleec.value.type == VType::Null || calleec.value.type == VType::Undefined)) {
                    return Completion::Norm(Value::Undef());
                }
                return Completion::Thr("value is not a function");
            }
            std::vector<Value> args;
            for (size_t i = 0; i < n.args.size(); ++i) {
                const auto &a = n.args[i];
                Completion c = EvalExpr(interp, *a, env);
                if (c.IsAbrupt()) return c;
                const bool spread = i < n.arg_spread.size() && n.arg_spread[i];
                if (spread) {
                    if (c.value.type != VType::Object || !c.value.obj || !c.value.obj->is_array)
                        return Completion::Thr("call spread requires an array");
                    const long length = ArrayLength(c.value.obj);
                    for (long j = 0; j < length; ++j) args.push_back(GetProp(c.value.obj, std::to_string(j)));
                } else {
                    args.push_back(c.value);
                }
            }
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
                if (!class_value.value.obj->is_function) return Completion::Thr("value is not a class constructor");
                return CallFunction(interp, class_value.value.obj, args);
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
            Value instance_value = Value::Obj(instance);
            Value constructor = GetProp(class_value.value.obj, "constructor");
            if (constructor.type == VType::Object && constructor.obj && constructor.obj->is_function) {
                Completion constructed = CallFunction(interp, constructor.obj, args, &instance_value);
                if (constructed.IsAbrupt()) return constructed;
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
            if (value.type != VType::Object || !value.obj || !value.obj->is_array)
                return Completion::Thr("array destructuring requires an array");
            for (size_t i = 0; i < pattern.elements.size(); ++i) {
                if (!pattern.elements[i]) continue;
                Completion bound = BindPattern(interp, *pattern.elements[i], GetProp(value.obj, std::to_string(i)), env);
                if (bound.IsAbrupt()) return bound;
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
            for (const auto &d : n.declarators) {
                Value v = Value::Undef();
                if (d.second) {
                    Completion c = EvalExpr(interp, *d.second, env);
                    if (c.IsAbrupt()) return c;
                    v = c.value;
                }
                env->Define(d.first, v);
            }
            for (const auto &d : n.pattern_declarators) {
                Value v = Value::Undef();
                if (d.second) {
                    Completion c = EvalExpr(interp, *d.second, env);
                    if (c.IsAbrupt()) return c;
                    v = c.value;
                }
                Completion bound = BindPattern(interp, *d.first, v, env);
                if (bound.IsAbrupt()) return bound;
            }
            return Completion::Norm();
        }
        case NodeKind::FunctionDecl:
            return Completion::Norm();  // already bound by ExecBlockBody's hoisting pass
        case NodeKind::Yield: {
            if (!interp.yield_values) return Completion::Thr("yield outside generator");
            Value value = Value::Undef();
            if (n.a) {
                Completion yielded = EvalExpr(interp, *n.a, env);
                if (yielded.IsAbrupt()) return yielded;
                value = yielded.value;
            }
            interp.yield_values->push_back(std::move(value));
            return Completion::Norm();
        }
        case NodeKind::Class: {
            ObjectPtr base_class;
            if (n.a) {
                Completion base = EvalExpr(interp, *n.a, env);
                if (base.IsAbrupt()) return base;
                if (base.value.type != VType::Object || !base.value.obj || !base.value.obj->is_class)
                    return Completion::Thr("class extends requires a class");
                base_class = base.value.obj;
            }
            auto klass = std::make_shared<ObjectData>();
            klass->is_class = true;
            klass->class_node = &n;
            klass->private_field_names = n.class_private_fields;
            klass->prototype = std::make_shared<ObjectData>();
            if (base_class) klass->prototype->prototype = base_class->prototype;
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
        case NodeKind::For: {
            EnvPtr loop_env = std::make_shared<Environment>();
            loop_env->parent = env;
            if (n.op == "of" || n.op == "in") {
                Completion source = EvalExpr(interp, *n.a, loop_env);
                if (source.IsAbrupt()) return source;
                if (source.value.type != VType::Object || !source.value.obj) {
                    return Completion::Thr("for..." + n.op + " requires an object");
                }
                std::vector<Value> values;
                if (n.op == "of") {
                    if (source.value.obj->is_array) {
                        const long length = ArrayLength(source.value.obj);
                        for (long i = 0; i < length; ++i) values.push_back(GetProp(source.value.obj, std::to_string(i)));
                    } else if (source.value.obj->is_set) {
                        for (const auto &entry : source.value.obj->collection_entries) values.push_back(entry.first);
                    } else if (source.value.obj->is_map) {
                        for (const auto &entry : source.value.obj->collection_entries) {
                            auto pair = std::make_shared<ObjectData>(); pair->is_array = true;
                            pair->props["0"] = entry.first; pair->props["1"] = entry.second; pair->props["length"] = Value::Num(2);
                            values.push_back(Value::Obj(pair));
                        }
                    } else return Completion::Thr("for...of requires an iterable");
                } else {
                    for (const auto &[key, value] : source.value.obj->props) {
                        (void)value;
                        if (source.value.obj->is_array && key == "length") continue;
                        values.push_back(Value::Str(key));
                    }
                }
                for (Value &value : values) {
                    Completion guard;
                    if (interp.StepGuard(guard)) return guard;
                    EnvPtr iteration_env = std::make_shared<Environment>();
                    iteration_env->parent = loop_env;
                    if (n.boolean) {
                        iteration_env->Define(n.name, std::move(value));
                    } else {
                        Value *slot = loop_env->Find(n.name);
                        if (slot) *slot = std::move(value);
                        else interp.global->Define(n.name, std::move(value));
                    }
                    Completion body = ExecStmt(interp, *n.then_branch, iteration_env);
                    if (body.type == CompletionType::Break) {
                        if (body.label.empty() || body.label == n.name) break;
                        return body;
                    }
                    if (body.type == CompletionType::Continue && !body.label.empty() && body.label != n.name) return body;
                    if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
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
                if (!n.name.empty()) catch_scope->Define(n.name, result.value);
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

Value WrapDomNode(HtmlDoc &doc, DomNode *node) {
    if (!node) return Value::MakeNull();
    auto wrapper = std::make_shared<ObjectData>();
    wrapper->dom_node = node;
    wrapper->owner_doc = &doc;
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
        if (args.empty() || args[0].type != VType::String || doc.resource_base_dir.empty()) {
            SettlePromise(interp, promise, 2, Value::Str("fetch requires a local document resource")); return Value::Obj(promise);
        }
        const std::string url = args[0].str;
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
            if (url.type != VType::String || !ResolveDocumentResource(doc.resource_base_dir, url.str, path)) {
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
        std::string flags = args.size() < 2 ? "" : ToDisplayString(args[1]);
        auto regex = std::make_shared<mep_regex::Regex>(pattern, flags.find('i') != std::string::npos);
        if (!regex->ok()) { threw = true; err = "invalid RegExp: " + regex->error(); return Value::Undef(); }
        auto value = std::make_shared<ObjectData>();
        value->is_regexp = true;
        value->regexp_global = flags.find('g') != std::string::npos;
        value->props["lastIndex"] = Value::Num(0);
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
    auto iterator_symbol = std::make_shared<ObjectData>(); iterator_symbol->is_symbol = true; iterator_symbol->props["description"] = Value::Str("Symbol.iterator");
    symbol.obj->props["iterator"] = Value::Obj(iterator_symbol);
    global->Define("Symbol", symbol);

    global->Define("Proxy", MakeNativeFn([](const std::vector<Value> &args, bool &threw, std::string &err) {
        if (args.size() < 2 || args[0].type != VType::Object || !args[0].obj || args[1].type != VType::Object || !args[1].obj) { threw = true; err = "Proxy requires target and handler objects"; return Value::Undef(); }
        auto proxy = std::make_shared<ObjectData>(); proxy->proxy_target = args[0].obj; proxy->proxy_handler = args[1].obj;
        return Value::Obj(proxy);
    }));

    auto make_error = [](const std::string &name) {
        return MakeNativeFn([name](const std::vector<Value> &args, bool &, std::string &) {
            auto error = std::make_shared<ObjectData>();
            error->props["name"] = Value::Str(name);
            error->props["message"] = Value::Str(args.empty() ? "" : ToDisplayString(args[0]));
            error->props["stack"] = Value::Str(name + ": " + (args.empty() ? "" : ToDisplayString(args[0])));
            return Value::Obj(error);
        });
    };
    global->Define("Error", make_error("Error"));
    global->Define("TypeError", make_error("TypeError"));
    global->Define("RangeError", make_error("RangeError"));
    global->Define("SyntaxError", make_error("SyntaxError"));

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
    json->props["stringify"] = MakeNativeFn([value_to_json](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty()) return Value::Undef();
        Json encoded;
        return (*value_to_json)(args[0], encoded) ? Value::Str(encoded.dump()) : Value::Undef();
    });
    global->Define("JSON", Value::Obj(json));

    auto array_ctor = std::make_shared<ObjectData>();
    array_ctor->props["isArray"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        return Value::Bool(!args.empty() && args[0].type == VType::Object && args[0].obj && args[0].obj->is_array);
    });
    global->Define("Array", Value::Obj(array_ctor));

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
            for (const auto &[key, value] : args[0].obj->props) if (!(args[0].obj->is_array && key == "length")) values.push_back(Value::Str(key));
        return make_array(values);
    });
    object_ctor->props["values"] = MakeNativeFn([make_array](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> values;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj)
            for (const auto &[key, value] : args[0].obj->props) if (!(args[0].obj->is_array && key == "length")) values.push_back(value);
        return make_array(values);
    });
    object_ctor->props["entries"] = MakeNativeFn([make_array](const std::vector<Value> &args, bool &, std::string &) {
        std::vector<Value> entries;
        if (!args.empty() && args[0].type == VType::Object && args[0].obj)
            for (const auto &[key, value] : args[0].obj->props) if (!(args[0].obj->is_array && key == "length")) entries.push_back(make_array({Value::Str(key), value}));
        return make_array(entries);
    });
    object_ctor->props["assign"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj) return Value::Undef();
        for (size_t i = 1; i < args.size(); ++i) if (args[i].type == VType::Object && args[i].obj)
            for (const auto &[key, value] : args[i].obj->props) if (!(args[i].obj->is_array && key == "length")) args[0].obj->props[key] = value;
        return args[0];
    });
    object_ctor->props["create"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        auto object = std::make_shared<ObjectData>();
        if (!args.empty() && args[0].type == VType::Object) object->prototype = args[0].obj;
        return Value::Obj(object);
    });
    object_ctor->props["getPrototypeOf"] = MakeNativeFn([](const std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::Object || !args[0].obj || !args[0].obj->prototype) return Value::MakeNull();
        return Value::Obj(args[0].obj->prototype);
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
        const std::string key = ToDisplayString(args[1]);
        if (args[2].type == VType::Object && args[2].obj) {
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
        descriptor->props["enumerable"] = Value::Bool(true);
        descriptor->props["configurable"] = Value::Bool(!args[0].obj->frozen);
        return Value::Obj(descriptor);
    });
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
        Value callback = GetProp(window, "onpopstate");
        if (callback.type == VType::Object && callback.obj && callback.obj->is_function) {
            auto event = std::make_shared<ObjectData>(); event->props["type"] = Value::Str("popstate"); event->props["state"] = (*entries)[index].first;
            std::vector<Value> args{Value::Obj(event)}; (void)CallFunction(interp, callback.obj, args, nullptr);
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
    window->props["location"] = Value::Obj(location);
    window->location_object = location;
    window->props["history"] = Value::Obj(history);
    global->Define("location", Value::Obj(location));
    global->Define("history", Value::Obj(history));
    global->Define("window", Value::Obj(window));
}

}  // namespace

void RunScripts(HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log,
                 const std::function<void(const std::string &)> &on_error) {
    // All script tags in one document share the same global scope. Keep each
    // parsed program alive until the whole sequence is done too: a function
    // declared by an early script may be called by a later script and its AST
    // must not dangle between iterations.
    Interpreter interp;
    interp.global = std::make_shared<Environment>();
    SetupGlobals(interp, doc, on_console_log);
    std::vector<NodePtr> programs;
    for (const std::string &script : doc.scripts) {
        Parser parser(script);
        NodePtr program = parser.ParseProgram();
        if (!parser.ok) {
            on_error("script parse error: " + parser.error);
            continue;
        }
        programs.push_back(std::move(program));
        EnvPtr scope = interp.global;
        Completion result = ExecBlockBody(interp, programs.back()->body, scope);
        if (result.type == CompletionType::Throw) {
            on_error("script error: " + ToDisplayString(result.value));
        }
        Completion microtasks = DrainMicrotasks(interp);
        if (microtasks.type == CompletionType::Throw) {
            on_error("microtask error: " + ToDisplayString(microtasks.value));
        }
        Completion timers = RunDueTimers(interp);
        if (timers.type == CompletionType::Throw) {
            on_error("timer error: " + ToDisplayString(timers.value));
        }
    }
    // Attribute/class/tree mutations can affect inherited and selector based
    // styles. Layout reads ComputedStyle directly, so refresh it once after
    // the document's synchronous script sequence completes.
    ComputeStyles(doc);
}
