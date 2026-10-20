// JSON for mep-ts-generate: see json.h.
#include "json.h"

#include <cstdlib>

namespace tsgen {

const JsonValue *JsonValue::Get(const std::string &key) const {
    if (type != Type::Object) return nullptr;
    // serde_json's preserve_order map keeps the last value for a repeated key
    // in the first key's position.
    const JsonValue *found = nullptr;
    for (const auto &kv : object)
        if (kv.first == key) found = &kv.second;
    return found;
}

int64_t JsonValue::AsInt() const { return std::strtoll(number.c_str(), nullptr, 10); }

namespace {

struct Parser {
    const std::string &s;
    size_t i = 0;
    std::string error;

    void SkipSpace() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool Fail(const std::string &message) {
        if (error.empty()) error = message + " at byte " + std::to_string(i);
        return false;
    }
    static void PutUtf8(std::string &out, uint32_t c) {
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
    }
    bool Hex4(uint32_t &out) {
        if (i + 4 > s.size()) return Fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("bad \\u escape");
        }
        return true;
    }
    bool String(std::string &out) {
        i++;  // opening quote
        while (true) {
            if (i >= s.size()) return Fail("unterminated string");
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return Fail("unterminated escape");
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t c1;
                    if (!Hex4(c1)) return false;
                    if (c1 >= 0xD800 && c1 < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        uint32_t c2;
                        if (!Hex4(c2)) return false;
                        if (c2 >= 0xDC00 && c2 < 0xE000) {
                            PutUtf8(out, 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00));
                            break;
                        }
                        i = save;
                    }
                    if (c1 >= 0xD800 && c1 < 0xE000) return Fail("lone surrogate in string");
                    PutUtf8(out, c1);
                    break;
                }
                default:
                    return Fail("bad escape");
            }
        }
    }
    bool Value(JsonValue &out) {
        SkipSpace();
        if (i >= s.size()) return Fail("unexpected end of input");
        char c = s[i];
        if (c == '{') {
            out.type = JsonValue::Type::Object;
            i++;
            SkipSpace();
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                SkipSpace();
                if (i >= s.size() || s[i] != '"') return Fail("expected key");
                std::string key;
                if (!String(key)) return false;
                SkipSpace();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                i++;
                JsonValue v;
                if (!Value(v)) return false;
                bool replaced = false;
                for (auto &kv : out.object) {
                    if (kv.first == key) {
                        kv.second = std::move(v);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) out.object.push_back({std::move(key), std::move(v)});
                SkipSpace();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    i++;
                    return true;
                }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out.type = JsonValue::Type::Array;
            i++;
            SkipSpace();
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                JsonValue v;
                if (!Value(v)) return false;
                out.array.push_back(std::move(v));
                SkipSpace();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    i++;
                    return true;
                }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return String(out.string);
        }
        if (s.compare(i, 4, "true") == 0) {
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            i += 4;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            out.type = JsonValue::Type::Bool;
            i += 5;
            return true;
        }
        if (s.compare(i, 4, "null") == 0) {
            i += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            out.type = JsonValue::Type::Number;
            size_t start = i;
            if (s[i] == '-') i++;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '+' || s[i] == '-'))
                i++;
            out.number = s.substr(start, i - start);
            return true;
        }
        return Fail("unexpected character");
    }
};

void EscapeString(std::string &out, const std::string &s) {
    out += '"';
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char *hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void Write(std::string &out, const JsonValue &v, size_t indent) {
    switch (v.type) {
        case JsonValue::Type::Null:
            out += "null";
            break;
        case JsonValue::Type::Bool:
            out += v.boolean ? "true" : "false";
            break;
        case JsonValue::Type::Number:
            out += v.number;
            break;
        case JsonValue::Type::String:
            EscapeString(out, v.string);
            break;
        case JsonValue::Type::Array:
            if (v.array.empty()) {
                out += "[]";
                break;
            }
            out += "[\n";
            for (size_t k = 0; k < v.array.size(); k++) {
                out.append((indent + 1) * 2, ' ');
                Write(out, v.array[k], indent + 1);
                if (k + 1 < v.array.size()) out += ',';
                out += '\n';
            }
            out.append(indent * 2, ' ');
            out += ']';
            break;
        case JsonValue::Type::Object:
            if (v.object.empty()) {
                out += "{}";
                break;
            }
            out += "{\n";
            for (size_t k = 0; k < v.object.size(); k++) {
                out.append((indent + 1) * 2, ' ');
                EscapeString(out, v.object[k].first);
                out += ": ";
                Write(out, v.object[k].second, indent + 1);
                if (k + 1 < v.object.size()) out += ',';
                out += '\n';
            }
            out.append(indent * 2, ' ');
            out += '}';
            break;
    }
}

}  // namespace

bool ParseJson(const std::string &text, JsonValue &out, std::string &error) {
    Parser p{text, 0, {}};
    if (!p.Value(out)) {
        error = p.error;
        return false;
    }
    p.SkipSpace();
    if (p.i != text.size()) {
        error = "trailing characters at byte " + std::to_string(p.i);
        return false;
    }
    return true;
}

std::string PrettyJson(const JsonValue &v) {
    std::string out;
    Write(out, v, 0);
    return out;
}

}  // namespace tsgen
