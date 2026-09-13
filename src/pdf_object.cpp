#include "pdf_object.h"

#include <cctype>
#include <cstdlib>

namespace pdfobj {

namespace {

bool IsWhitespace(unsigned char c) {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

bool IsDelimiter(unsigned char c) {
    switch (c) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case '/':
        case '%':
            return true;
        default:
            return false;
    }
}

bool IsRegular(unsigned char c) { return !IsWhitespace(c) && !IsDelimiter(c); }

unsigned char PeekByte(const unsigned char *data, size_t len, size_t pos) {
    return pos < len ? data[pos] : 0;
}

// Matches a literal ASCII keyword (e.g. "obj", "true") at data[pos],
// requiring it not be immediately followed by another regular byte (so
// "trueish" doesn't match "true") -- does NOT skip leading whitespace.
bool MatchKeyword(const unsigned char *data, size_t len, size_t pos, const char *keyword) {
    size_t klen = std::char_traits<char>::length(keyword);
    if (pos + klen > len) return false;
    for (size_t i = 0; i < klen; ++i) {
        if (data[pos + i] != static_cast<unsigned char>(keyword[i])) return false;
    }
    if (pos + klen < len && IsRegular(data[pos + klen])) return false;
    return true;
}

int HexDigitValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes a `(...)`-delimited literal string starting at data[pos]
// (pointing at the opening '('), handling balanced nested parens,
// backslash escapes (\n \r \t \b \f \( \) \\, a backslash-newline line
// continuation that contributes no character, and up to 3-digit octal
// \ddd), and bare CR/CR-LF normalized to LF per spec 7.3.4.2. Advances
// pos past the closing ')'.
bool ParseLiteralString(const unsigned char *data, size_t len, size_t &pos, std::string *out) {
    if (PeekByte(data, len, pos) != '(') return false;
    ++pos;
    int depth = 1;
    out->clear();
    while (pos < len) {
        unsigned char c = data[pos];
        if (c == '\\') {
            ++pos;
            if (pos >= len) break;
            unsigned char e = data[pos];
            switch (e) {
                case 'n':
                    out->push_back('\n');
                    ++pos;
                    break;
                case 'r':
                    out->push_back('\r');
                    ++pos;
                    break;
                case 't':
                    out->push_back('\t');
                    ++pos;
                    break;
                case 'b':
                    out->push_back('\b');
                    ++pos;
                    break;
                case 'f':
                    out->push_back('\f');
                    ++pos;
                    break;
                case '(':
                case ')':
                case '\\':
                    out->push_back(static_cast<char>(e));
                    ++pos;
                    break;
                case '\r':
                    // Backslash-newline line continuation: contributes nothing.
                    ++pos;
                    if (pos < len && data[pos] == '\n') ++pos;
                    break;
                case '\n':
                    ++pos;
                    break;
                default:
                    if (e >= '0' && e <= '7') {
                        int value = 0;
                        int digits = 0;
                        while (digits < 3 && pos < len && data[pos] >= '0' && data[pos] <= '7') {
                            value = value * 8 + (data[pos] - '0');
                            ++pos;
                            ++digits;
                        }
                        out->push_back(static_cast<char>(value & 0xFF));
                    } else {
                        // Unknown escape: per spec, the backslash is ignored and
                        // the following char is literal.
                        out->push_back(static_cast<char>(e));
                        ++pos;
                    }
                    break;
            }
        } else if (c == '(') {
            ++depth;
            out->push_back('(');
            ++pos;
        } else if (c == ')') {
            --depth;
            ++pos;
            if (depth == 0) return true;
            out->push_back(')');
        } else if (c == '\r') {
            out->push_back('\n');
            ++pos;
            if (pos < len && data[pos] == '\n') ++pos;
        } else {
            out->push_back(static_cast<char>(c));
            ++pos;
        }
    }
    return true;  // unterminated string: tolerate, return what we have (matches this codebase's lenient-parse convention)
}

// Decodes a `<...>`-delimited hex string starting at data[pos] (pointing
// at the opening '<' -- caller must have already ruled out "<<"). Ignores
// interior whitespace; an odd trailing digit is padded with an implicit
// trailing 0 per spec 7.3.4.3.
bool ParseHexString(const unsigned char *data, size_t len, size_t &pos, std::string *out) {
    if (PeekByte(data, len, pos) != '<') return false;
    ++pos;
    out->clear();
    int hi = -1;
    while (pos < len && data[pos] != '>') {
        int v = HexDigitValue(data[pos]);
        ++pos;
        if (v < 0) continue;  // skip whitespace/garbage inside the hex string
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back(static_cast<char>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) out->push_back(static_cast<char>(hi << 4));
    if (pos < len && data[pos] == '>') ++pos;
    return true;
}

// Decodes a `/Name` starting at data[pos] (pointing at the '/'),
// resolving #xx hex escapes per spec 7.3.5. Advances pos past the last
// regular-character byte of the name.
bool ParseName(const unsigned char *data, size_t len, size_t &pos, std::string *out) {
    if (PeekByte(data, len, pos) != '/') return false;
    ++pos;
    out->clear();
    while (pos < len && IsRegular(data[pos])) {
        if (data[pos] == '#' && pos + 2 < len) {
            int hi = HexDigitValue(data[pos + 1]);
            int lo = HexDigitValue(data[pos + 2]);
            if (hi >= 0 && lo >= 0) {
                out->push_back(static_cast<char>((hi << 4) | lo));
                pos += 3;
                continue;
            }
        }
        out->push_back(static_cast<char>(data[pos]));
        ++pos;
    }
    return true;
}

// Parses a numeric token (optional sign, digits, optional '.', digits --
// PDF's own real-number grammar has no exponent, but a handful of
// nonconforming producers emit one anyway; tolerated here rather than
// stopping short and misparsing the rest as a separate token). Returns
// false if data[pos] can't start a number.
bool ParseNumberToken(const unsigned char *data, size_t len, size_t &pos, Object *out) {
    size_t start = pos;
    if (pos < len && (data[pos] == '+' || data[pos] == '-')) ++pos;
    bool any_digit = false;
    bool is_real = false;
    while (pos < len && std::isdigit(data[pos])) {
        ++pos;
        any_digit = true;
    }
    if (pos < len && data[pos] == '.') {
        is_real = true;
        ++pos;
        while (pos < len && std::isdigit(data[pos])) {
            ++pos;
            any_digit = true;
        }
    }
    if (pos < len && (data[pos] == 'e' || data[pos] == 'E')) {
        size_t save = pos;
        size_t p = pos + 1;
        if (p < len && (data[p] == '+' || data[p] == '-')) ++p;
        if (p < len && std::isdigit(data[p])) {
            is_real = true;
            pos = p;
            while (pos < len && std::isdigit(data[pos])) ++pos;
        } else {
            pos = save;
        }
    }
    if (!any_digit) {
        pos = start;
        return false;
    }
    std::string token(reinterpret_cast<const char *>(data + start), pos - start);
    if (is_real) {
        out->type = Type::Real;
        out->real_val = std::strtod(token.c_str(), nullptr);
    } else {
        out->type = Type::Int;
        out->int_val = std::strtoll(token.c_str(), nullptr, 10);
    }
    return true;
}

bool ParseArray(const unsigned char *data, size_t len, size_t &pos, Object *out) {
    if (PeekByte(data, len, pos) != '[') return false;
    ++pos;
    out->type = Type::Array;
    out->array_val.clear();
    while (true) {
        SkipWhitespaceAndComments(data, len, pos);
        if (pos >= len) break;
        if (data[pos] == ']') {
            ++pos;
            break;
        }
        Object element;
        if (!ParseObject(data, len, pos, &element)) break;  // tolerant: stop at first unparseable element
        out->array_val.push_back(std::move(element));
    }
    return true;
}

bool ParseDictOrStreamlessDict(const unsigned char *data, size_t len, size_t &pos, Object *out) {
    if (pos + 1 >= len || data[pos] != '<' || data[pos + 1] != '<') return false;
    pos += 2;
    out->type = Type::Dict;
    out->dict_val.clear();
    while (true) {
        SkipWhitespaceAndComments(data, len, pos);
        if (pos + 1 < len && data[pos] == '>' && data[pos + 1] == '>') {
            pos += 2;
            break;
        }
        if (pos >= len || data[pos] != '/') break;  // tolerant: malformed dict, stop here
        std::string key;
        ParseName(data, len, pos, &key);
        SkipWhitespaceAndComments(data, len, pos);
        Object value;
        if (!ParseObject(data, len, pos, &value)) break;
        out->dict_val[key] = std::move(value);
    }
    return true;
}

}  // namespace

void SkipWhitespaceAndComments(const unsigned char *data, size_t len, size_t &pos) {
    while (pos < len) {
        if (IsWhitespace(data[pos])) {
            ++pos;
        } else if (data[pos] == '%') {
            while (pos < len && data[pos] != '\n' && data[pos] != '\r') ++pos;
        } else {
            break;
        }
    }
}

double Object::AsDouble(double def) const {
    if (type == Type::Int) return static_cast<double>(int_val);
    if (type == Type::Real) return real_val;
    return def;
}

long long Object::AsInt(long long def) const {
    if (type == Type::Int) return int_val;
    if (type == Type::Real) return static_cast<long long>(real_val);
    return def;
}

std::string Object::AsString(const std::string &def) const {
    if (type == Type::Name || type == Type::String) return str_val;
    return def;
}

const Object *Object::Find(const std::string &key) const {
    if (type != Type::Dict) return nullptr;
    auto it = dict_val.find(key);
    return it == dict_val.end() ? nullptr : &it->second;
}

bool ParseObject(const unsigned char *data, size_t len, size_t &pos, Object *out) {
    *out = Object();
    SkipWhitespaceAndComments(data, len, pos);
    if (pos >= len) return false;
    unsigned char c = data[pos];

    if (c == '/') {
        out->type = Type::Name;
        return ParseName(data, len, pos, &out->str_val);
    }
    if (c == '(') {
        out->type = Type::String;
        return ParseLiteralString(data, len, pos, &out->str_val);
    }
    if (c == '[') return ParseArray(data, len, pos, out);
    if (c == '<') {
        if (pos + 1 < len && data[pos + 1] == '<') return ParseDictOrStreamlessDict(data, len, pos, out);
        out->type = Type::String;
        return ParseHexString(data, len, pos, &out->str_val);
    }
    if (MatchKeyword(data, len, pos, "true")) {
        out->type = Type::Bool;
        out->bool_val = true;
        pos += 4;
        return true;
    }
    if (MatchKeyword(data, len, pos, "false")) {
        out->type = Type::Bool;
        out->bool_val = false;
        pos += 5;
        return true;
    }
    if (MatchKeyword(data, len, pos, "null")) {
        out->type = Type::Null;
        pos += 4;
        return true;
    }
    if (c == '+' || c == '-' || c == '.' || std::isdigit(c)) {
        // Could be a plain number, or the start of "N G R" (indirect
        // reference) -- only an unsigned-integer-looking token can be
        // the first half of a reference, so try that lookahead before
        // committing to a plain number.
        Object first;
        if (!ParseNumberToken(data, len, pos, &first)) return false;
        if (first.type == Type::Int && first.int_val >= 0) {
            size_t lookahead = pos;
            SkipWhitespaceAndComments(data, len, lookahead);
            Object second;
            if (ParseNumberToken(data, len, lookahead, &second) && second.type == Type::Int &&
                second.int_val >= 0) {
                SkipWhitespaceAndComments(data, len, lookahead);
                if (MatchKeyword(data, len, lookahead, "R")) {
                    out->type = Type::Reference;
                    out->ref_val.num = static_cast<int>(first.int_val);
                    out->ref_val.gen = static_cast<int>(second.int_val);
                    pos = lookahead + 1;
                    return true;
                }
            }
        }
        *out = first;
        return true;
    }
    // Unrecognized byte (stray delimiter like ')'/']'/'}'/'>' with no
    // opener, or garbage) -- tolerant failure, matching this module's
    // documented "caller decides" contract.
    return false;
}

bool ParseIndirectObject(const unsigned char *data, size_t len, size_t pos, IndirectObject *out,
                          const LengthResolver &resolve_length) {
    *out = IndirectObject();
    size_t p = pos;
    SkipWhitespaceAndComments(data, len, p);
    Object num_obj;
    if (!ParseNumberToken(data, len, p, &num_obj) || num_obj.type != Type::Int) return false;
    SkipWhitespaceAndComments(data, len, p);
    Object gen_obj;
    if (!ParseNumberToken(data, len, p, &gen_obj) || gen_obj.type != Type::Int) return false;
    SkipWhitespaceAndComments(data, len, p);
    if (!MatchKeyword(data, len, p, "obj")) return false;
    p += 3;

    out->num = static_cast<int>(num_obj.int_val);
    out->gen = static_cast<int>(gen_obj.int_val);

    if (!ParseObject(data, len, p, &out->value)) {
        // Tolerant: header parsed fine, value didn't -- leave value Null
        // rather than failing the whole object (matches this codebase's
        // "skip bad content" convention).
        return true;
    }

    SkipWhitespaceAndComments(data, len, p);
    if (MatchKeyword(data, len, p, "stream")) {
        p += 6;
        // Per spec 7.3.8.1: "stream" is followed by CRLF or bare LF (not
        // bare CR) before the data starts. Tolerate a bare CR too, and
        // tolerate stray spaces before the line break -- real-world
        // producers aren't always spec-exact.
        while (p < len && (data[p] == ' ' || data[p] == '\t')) ++p;
        if (p < len && data[p] == '\r') ++p;
        if (p < len && data[p] == '\n') ++p;

        out->has_stream = true;
        out->stream_offset = p;

        long long length = -1;
        const Object *len_obj = out->value.Find("Length");
        if (len_obj) {
            if (len_obj->type == Type::Int) {
                length = len_obj->int_val;
            } else if (len_obj->type == Type::Reference && resolve_length) {
                long long resolved = 0;
                if (resolve_length(len_obj->ref_val.num, len_obj->ref_val.gen, &resolved)) length = resolved;
            }
        }

        bool length_ok = length >= 0 && p + static_cast<size_t>(length) <= len;
        if (length_ok) {
            // Sanity-check that "endstream" actually follows (within a
            // few bytes, allowing an EOL before it) -- an indirect
            // /Length that resolves but points at the wrong place is
            // exactly the kind of malformed-but-real-world case worth
            // falling back on rather than trusting blindly.
            size_t check = p + static_cast<size_t>(length);
            size_t probe = check;
            while (probe < len && IsWhitespace(data[probe]) && probe < check + 2) ++probe;
            if (!MatchKeyword(data, len, probe, "endstream")) length_ok = false;
        }

        if (length_ok) {
            out->stream_length = static_cast<size_t>(length);
            p += static_cast<size_t>(length);
        } else {
            // Fallback: scan forward for the next "endstream" keyword.
            size_t scan = p;
            size_t found = std::string::npos;
            while (scan + 9 <= len) {
                if (MatchKeyword(data, len, scan, "endstream")) {
                    found = scan;
                    break;
                }
                ++scan;
            }
            if (found == std::string::npos) {
                out->stream_length = len - p;  // truly unterminated: take the rest of the buffer
                p = len;
            } else {
                size_t stream_end = found;
                // Trim a single trailing EOL immediately before "endstream", per spec.
                if (stream_end > p && data[stream_end - 1] == '\n') --stream_end;
                if (stream_end > p && data[stream_end - 1] == '\r') --stream_end;
                out->stream_length = stream_end - p;
                p = found;
            }
        }
        SkipWhitespaceAndComments(data, len, p);
        if (MatchKeyword(data, len, p, "endstream")) p += 9;
    }

    SkipWhitespaceAndComments(data, len, p);
    // "endobj" is expected next; not required for a successful parse
    // (tolerant of a missing/misplaced endobj, same reasoning as above).
    return true;
}

}  // namespace pdfobj
