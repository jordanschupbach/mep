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

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), num_(n) {}
    Json(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(const char *s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Json Array() {
        Json j;
        j.type_ = Type::Array;
        return j;
    }
    static Json Object() {
        Json j;
        j.type_ = Type::Object;
        return j;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_double(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
    int as_int(int fallback = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : fallback; }
    const std::string &as_string() const {
        static const std::string kEmpty;
        return type_ == Type::String ? str_ : kEmpty;
    }
    std::string as_string(const std::string &fallback) const { return type_ == Type::String ? str_ : fallback; }

    const std::vector<Json> &items() const { return arr_; }
    std::vector<Json> &items() { return arr_; }
    size_t size() const { return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0); }

    void push_back(Json v) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            arr_.clear();
        }
        arr_.push_back(std::move(v));
    }

    // Object field access. `operator[]` creates the key (object semantics)
    // if absent; `get`/`contains` are read-only lookups.
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

    bool contains(const std::string &key) const {
        if (type_ != Type::Object) return false;
        for (const auto &kv : obj_) {
            if (kv.first == key) return true;
        }
        return false;
    }

    const Json &get(const std::string &key) const {
        static const Json kNull;
        if (type_ == Type::Object) {
            for (const auto &kv : obj_) {
                if (kv.first == key) return kv.second;
            }
        }
        return kNull;
    }

    const std::vector<std::pair<std::string, Json>> &fields() const { return obj_; }

    // Serializes to compact JSON text (no pretty-printing -- this is meant
    // for persisted state files and wire messages, not human editing).
    std::string dump() const {
        std::string out;
        DumpTo(out);
        return out;
    }

    // Parses `text` into `out`. Returns false (leaving `out` untouched) on
    // malformed input rather than throwing -- callers (persisted state
    // files, network/process input) should treat a parse failure as "no
    // data" rather than crash the editor.
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

    static void SkipWs(const std::string &s, size_t &pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) pos++;
    }

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
                        pos += 4;
                        // Minimal UTF-8 encode (no surrogate-pair handling --
                        // sufficient for the ASCII/BMP text this app deals
                        // with; a \uXXXX astral surrogate pair round-trips
                        // as two independent replacement-ish codepoints
                        // rather than failing outright).
                        if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xE0 | (cp >> 12));
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
