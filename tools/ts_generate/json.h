// JSON for mep-ts-generate: objects keep their keys in document order (a
// grammar's `rules` order is its symbol order, as serde_json's
// `preserve_order` feature keeps it), and Pretty() writes what serde_json's
// to_string_pretty does, byte for byte.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tsgen {

struct JsonMember;

struct JsonValue {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string number;  // the literal as written: integers stay exact
    std::string string;
    std::vector<JsonValue> array;
    std::vector<JsonMember> object;  // in document order

    // Out of line: JsonMember is incomplete here.
    JsonValue();
    ~JsonValue();
    JsonValue(const JsonValue &);
    JsonValue(JsonValue &&) noexcept;
    JsonValue &operator=(const JsonValue &);
    JsonValue &operator=(JsonValue &&) noexcept;

    const JsonValue *Get(const std::string &key) const;
    bool IsNull() const { return type == Type::Null; }
    int64_t AsInt() const;
    static JsonValue Str(std::string s) {
        JsonValue v;
        v.type = Type::String;
        v.string = std::move(s);
        return v;
    }
    static JsonValue Bool(bool b) {
        JsonValue v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }
    static JsonValue Arr() {
        JsonValue v;
        v.type = Type::Array;
        return v;
    }
    static JsonValue Obj() {
        JsonValue v;
        v.type = Type::Object;
        return v;
    }
    void Set(std::string key, JsonValue v);
};

struct JsonMember {
    std::string first;
    JsonValue second;
};

inline JsonValue::JsonValue() = default;
inline JsonValue::~JsonValue() = default;
inline JsonValue::JsonValue(const JsonValue &) = default;
inline JsonValue::JsonValue(JsonValue &&) noexcept = default;
inline JsonValue &JsonValue::operator=(const JsonValue &) = default;
inline JsonValue &JsonValue::operator=(JsonValue &&) noexcept = default;

inline void JsonValue::Set(std::string key, JsonValue v) { object.push_back({std::move(key), std::move(v)}); }

// Parses `text`; on a syntax error returns false with `error` set.
bool ParseJson(const std::string &text, JsonValue &out, std::string &error);

// serde_json::to_string_pretty
std::string PrettyJson(const JsonValue &v);

}  // namespace tsgen
