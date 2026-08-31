#ifndef MEP_JSON_H
#define MEP_JSON_H

// Minimal JSON value type + parser + serializer (NVIM_PARITY_PLAN.md Part I
// Phase 2). Hand-rolled rather than vendored: mep only ever needs to parse/
// build small documents (persisted state files, and later LSP/DAP wire
// messages), and a few hundred lines here keeps compile times far lower
// than pulling in a general-purpose library for that. Header-only, no
// external dependencies, works on both the native and wasm builds.
//
// Not a strict/validating parser: it accepts standard JSON (objects,
// arrays, strings with the common escapes, numbers, true/false/null) and
// is deliberately lenient about trailing garbage after the top-level value
// (callers pass exactly one document, e.g. one file's contents or one
// Content-Length-framed message body).

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    /** @brief Constructs a null JSON value. */
    Json() : type_(Type::Null) {}
    /** @brief Constructs a null JSON value from a `nullptr`.
     *  @param unnamed nullptr tag selecting this overload (unused). */
    Json(std::nullptr_t) : type_(Type::Null) {}
    /** @brief Constructs a boolean JSON value.
     *  @param b the boolean value to store. */
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    /** @brief Constructs a numeric JSON value from an int.
     *  @param n the integer value, stored internally as a double. */
    Json(int n) : type_(Type::Number), num_(n) {}
    /** @brief Constructs a numeric JSON value from a long long.
     *  @param n the integer value, converted to and stored as a double. */
    Json(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    /** @brief Constructs a numeric JSON value from a double.
     *  @param n the numeric value to store. */
    Json(double n) : type_(Type::Number), num_(n) {}
    /** @brief Constructs a string JSON value from a C string.
     *  @param s the null-terminated string to copy in. */
    Json(const char *s) : type_(Type::String), str_(s) {}
    /** @brief Constructs a string JSON value from a std::string.
     *  @param s the string to move in. */
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    /** @brief Creates a new empty JSON array value.
     *  @return the constructed array-typed Json. */
    static Json Array() {
        Json j;
        j.type_ = Type::Array;
        return j;
    }
    /** @brief Creates a new empty JSON object value.
     *  @return the constructed object-typed Json. */
    static Json Object() {
        Json j;
        j.type_ = Type::Object;
        return j;
    }

    /** @brief Returns this value's JSON type tag.
     *  @return the current Type. */
    Type type() const { return type_; }
    /** @brief Checks whether this value is JSON null.
     *  @return true if the type is Null. */
    bool is_null() const { return type_ == Type::Null; }
    /** @brief Checks whether this value is a JSON boolean.
     *  @return true if the type is Bool. */
    bool is_bool() const { return type_ == Type::Bool; }
    /** @brief Checks whether this value is a JSON number.
     *  @return true if the type is Number. */
    bool is_number() const { return type_ == Type::Number; }
    /** @brief Checks whether this value is a JSON string.
     *  @return true if the type is String. */
    bool is_string() const { return type_ == Type::String; }
    /** @brief Checks whether this value is a JSON array.
     *  @return true if the type is Array. */
    bool is_array() const { return type_ == Type::Array; }
    /** @brief Checks whether this value is a JSON object.
     *  @return true if the type is Object. */
    bool is_object() const { return type_ == Type::Object; }

    /** @brief Reads this value as a bool.
     *  @param fallback value to return if this is not a Bool.
     *  @return the stored boolean, or `fallback` if the type doesn't match. */
    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    /** @brief Reads this value as a double.
     *  @param fallback value to return if this is not a Number.
     *  @return the stored number, or `fallback` if the type doesn't match. */
    double as_double(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
    /** @brief Reads this value as an int, truncating any fractional part.
     *  @param fallback value to return if this is not a Number.
     *  @return the stored number cast to int, or `fallback` if the type doesn't match. */
    int as_int(int fallback = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : fallback; }
    /** @brief Reads this value as a string reference, without a caller-supplied fallback.
     *  @return the stored string, or a shared empty string if this is not a String. */
    const std::string &as_string() const {
        static const std::string kEmpty;
        return type_ == Type::String ? str_ : kEmpty;
    }
    /** @brief Reads this value as a string, with a caller-supplied fallback.
     *  @param fallback value to return if this is not a String.
     *  @return a copy of the stored string, or `fallback` if the type doesn't match. */
    std::string as_string(const std::string &fallback) const { return type_ == Type::String ? str_ : fallback; }

    /** @brief Returns the underlying array elements (read-only).
     *  @return const reference to the element vector; empty if this is not an Array. */
    const std::vector<Json> &items() const { return arr_; }
    /** @brief Returns the underlying array elements (mutable).
     *  @return mutable reference to the element vector. */
    std::vector<Json> &items() { return arr_; }
    /** @brief Returns the number of elements/fields this value holds.
     *  @return the array length, the object field count, or 0 for any other type. */
    size_t size() const { return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0); }

    /** @brief Appends a value to this array, converting this value to an array first if needed.
     *  @param v the value to append (moved in). */
    void push_back(Json v) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arr_.clear();
        }
        arr_.push_back(std::move(v));
    }

    // Object field access. `operator[]` creates the key (object semantics)
    // if absent; `get`/`contains` are read-only lookups.
    /** @brief Accesses (or creates) an object field by key, converting this value to an object first if needed.
     *  @param key the field name to look up or insert.
     *  @return reference to the field's value, newly-inserted (as null) if it was absent. */
    Json &operator[](const std::string &key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            obj_.clear();
        }
        for (auto &kv : obj_) {
            if (kv.first == key) return kv.second;
        }
        obj_.emplace_back(key, Json());
        return obj_.back().second;
    }

    /** @brief Checks whether this object has a field with the given key.
     *  @param key the field name to look up.
     *  @return true if this is an Object and `key` is present. */
    bool contains(const std::string &key) const {
        if (type_ != Type::Object) return false;
        for (const auto &kv : obj_) {
            if (kv.first == key) return true;
        }
        return false;
    }

    /** @brief Looks up an object field without creating it if absent.
     *  @param key the field name to look up.
     *  @return the field's value, or a shared null Json if this is not an Object or `key` is absent. */
    const Json &get(const std::string &key) const {
        static const Json kNull;
        if (type_ == Type::Object) {
            for (const auto &kv : obj_) {
                if (kv.first == key) return kv.second;
            }
        }
        return kNull;
    }

    /** @brief Returns the underlying object fields in insertion order (read-only).
     *  @return const reference to the key/value pair vector; empty if this is not an Object. */
    const std::vector<std::pair<std::string, Json>> &fields() const { return obj_; }

    // Serializes to compact JSON text (no pretty-printing -- this is meant
    // for persisted state files and wire messages, not human editing).
    /** @brief Serializes this value to compact JSON text.
     *  @return the serialized JSON document, with no pretty-printing. */
    std::string dump() const {
        std::string out;
        DumpTo(out);
        return out;
    }

    // Parses `text` into `out`. Returns false (leaving `out` untouched) on
    // malformed input rather than throwing -- callers (persisted state
    // files, network/process input) should treat a parse failure as "no
    // data" rather than crash the editor.
    /** @brief Parses `text` into `out`, without throwing on malformed input.
     *  @param text the JSON document text to parse.
     *  @param out receives the parsed value on success; left untouched on failure.
     *  @return true if `text` parsed as a valid JSON value, false otherwise. */
    static bool Parse(const std::string &text, Json *out) {
        size_t pos = 0;
        SkipWs(text, pos);
        Json result;
        if (!ParseValue(text, pos, &result)) return false;
        *out = std::move(result);
        return true;
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;

    /** @brief Recursively appends this value's compact JSON text to `out`.
     *  @param out the string to append the serialized JSON to. */
    void DumpTo(std::string &out) const {
        switch (type_) {
            case Type::Null:
                out += "null";
                break;
            case Type::Bool:
                out += bool_ ? "true" : "false";
                break;
            case Type::Number: {
                if (num_ == static_cast<long long>(num_) && std::fabs(num_) < 1e15) {
                    out += std::to_string(static_cast<long long>(num_));
                } else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%g", num_);
                    out += buf;
                }
                break;
            }
            case Type::String:
                DumpString(str_, out);
                break;
            case Type::Array: {
                out += '[';
                for (size_t i = 0; i < arr_.size(); i++) {
                    if (i > 0) out += ',';
                    arr_[i].DumpTo(out);
                }
                out += ']';
                break;
            }
            case Type::Object: {
                out += '{';
                for (size_t i = 0; i < obj_.size(); i++) {
                    if (i > 0) out += ',';
                    DumpString(obj_[i].first, out);
                    out += ':';
                    obj_[i].second.DumpTo(out);
                }
                out += '}';
                break;
            }
        }
    }

    /** @brief Appends `s` to `out` as a quoted, escaped JSON string literal.
     *  @param s the raw string to encode.
     *  @param out the string to append the encoded literal to. */
    static void DumpString(const std::string &s, std::string &out) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    /** @brief Advances `pos` past any run of JSON whitespace characters (space, tab, LF, CR).
     *  @param s the text being scanned.
     *  @param pos the cursor to advance in place. */
    static void SkipWs(const std::string &s, size_t &pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
    }

    /** @brief Parses one JSON value (object, array, string, true/false/null, or number) starting at `pos`.
     *  @param s the text being parsed.
     *  @param pos the cursor, advanced past the parsed value on success.
     *  @param out receives the parsed value on success.
     *  @return true on success, false if the input at `pos` is not a valid JSON value. */
    static bool ParseValue(const std::string &s, size_t &pos, Json *out) {
        SkipWs(s, pos);
        if (pos >= s.size()) return false;
        char c = s[pos];
        if (c == '{') return ParseObject(s, pos, out);
        if (c == '[') return ParseArray(s, pos, out);
        if (c == '"') {
            std::string str;
            if (!ParseString(s, pos, &str)) return false;
            *out = Json(std::move(str));
            return true;
        }
        if (s.compare(pos, 4, "true") == 0) {
            pos += 4;
            *out = Json(true);
            return true;
        }
        if (s.compare(pos, 5, "false") == 0) {
            pos += 5;
            *out = Json(false);
            return true;
        }
        if (s.compare(pos, 4, "null") == 0) {
            pos += 4;
            *out = Json();
            return true;
        }
        return ParseNumber(s, pos, out);
    }

    /** @brief Parses a JSON object starting at `pos` (which must point at the opening '{').
     *  @param s the text being parsed.
     *  @param pos the cursor, advanced past the closing '}' on success.
     *  @param out receives the parsed object on success.
     *  @return true on success, false on malformed object syntax. */
    static bool ParseObject(const std::string &s, size_t &pos, Json *out) {
        pos++;  // consume '{'
        Json obj = Json::Object();
        SkipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') {
            pos++;
            *out = std::move(obj);
            return true;
        }
        while (true) {
            SkipWs(s, pos);
            if (pos >= s.size() || s[pos] != '"') return false;
            std::string key;
            if (!ParseString(s, pos, &key)) return false;
            SkipWs(s, pos);
            if (pos >= s.size() || s[pos] != ':') return false;
            pos++;
            Json val;
            if (!ParseValue(s, pos, &val)) return false;
            obj.obj_.emplace_back(std::move(key), std::move(val));
            SkipWs(s, pos);
            if (pos >= s.size()) return false;
            if (s[pos] == ',') {
                pos++;
                continue;
            }
            if (s[pos] == '}') {
                pos++;
                break;
            }
            return false;
        }
        *out = std::move(obj);
        return true;
    }

    /** @brief Parses a JSON array starting at `pos` (which must point at the opening '[').
     *  @param s the text being parsed.
     *  @param pos the cursor, advanced past the closing ']' on success.
     *  @param out receives the parsed array on success.
     *  @return true on success, false on malformed array syntax. */
    static bool ParseArray(const std::string &s, size_t &pos, Json *out) {
        pos++;  // consume '['
        Json arr = Json::Array();
        SkipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') {
            pos++;
            *out = std::move(arr);
            return true;
        }
        while (true) {
            Json val;
            if (!ParseValue(s, pos, &val)) return false;
            arr.arr_.push_back(std::move(val));
            SkipWs(s, pos);
            if (pos >= s.size()) return false;
            if (s[pos] == ',') {
                pos++;
                continue;
            }
            if (s[pos] == ']') {
                pos++;
                break;
            }
            return false;
        }
        *out = std::move(arr);
        return true;
    }

    /** @brief Parses a JSON string literal starting at `pos` (which must point at the opening quote), decoding escapes (including \uXXXX surrogate pairs) to UTF-8.
     *  @param s the text being parsed.
     *  @param pos the cursor, advanced past the closing quote on success.
     *  @param out receives the decoded string on success.
     *  @return true on success, false on malformed string syntax. */
    static bool ParseString(const std::string &s, size_t &pos, std::string *out) {
        if (pos >= s.size() || s[pos] != '"') return false;
        pos++;
        std::string result;
        while (pos < s.size() && s[pos] != '"') {
            char c = s[pos];
            if (c == '\\') {
                pos++;
                if (pos >= s.size()) return false;
                switch (s[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'u': {
                        if (pos + 4 >= s.size()) return false;
                        unsigned int cp = static_cast<unsigned int>(std::strtoul(s.substr(pos + 1, 4).c_str(), nullptr, 16));
                        // A high surrogate (0xD800-0xDBFF) must be combined
                        // with an immediately-following low surrogate
                        // (\uDC00-\uDFFF) into one real codepoint before
                        // UTF-8 encoding it -- JSON (like JS) represents
                        // astral characters (e.g. emoji) as a surrogate
                        // *pair* of two \u escapes, neither independently a
                        // valid standalone codepoint. Decoding each half on
                        // its own would corrupt any such character. An
                        // unpaired surrogate (malformed input, or a lone
                        // high surrogate at end of string) falls through to
                        // the plain 3-byte encode below unchanged, same as
                        // before this fix -- only the common well-formed-
                        // pair case actually changes behavior.
                        size_t next_pos = pos + 5;
                        if (cp >= 0xD800 && cp <= 0xDBFF && next_pos + 5 < s.size() && s[next_pos] == '\\' &&
                            s[next_pos + 1] == 'u') {
                            unsigned int lo =
                                static_cast<unsigned int>(std::strtoul(s.substr(next_pos + 2, 4).c_str(), nullptr, 16));
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00);
                                next_pos += 6;
                            }
                        }
                        pos = next_pos - 1;  // the pos++ right after this switch brings it to next_pos
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
                pos++;
            } else {
                result += c;
                pos++;
            }
        }
        if (pos >= s.size()) return false;
        pos++;  // consume closing '"'
        *out = std::move(result);
        return true;
    }

    /** @brief Parses a JSON number literal starting at `pos`.
     *  @param s the text being parsed.
     *  @param pos the cursor, advanced past the parsed number on success.
     *  @param out receives the parsed number on success.
     *  @return true on success; always true if the leading digit check passed, since strtod tolerates the rest. */
    static bool ParseNumber(const std::string &s, size_t &pos, Json *out) {
        size_t start = pos;
        if (pos < s.size() && s[pos] == '-') pos++;
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
        }
        double val = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
        *out = Json(val);
        return true;
    }
};

#endif  // MEP_JSON_H
