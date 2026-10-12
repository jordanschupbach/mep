#include "mail.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

// The build's own OAuth clients (CMakeLists.txt); absent in builds that
// don't generate it (the unit test), which then have none.
#if __has_include("mail_oauth_clients.h")
#include "mail_oauth_clients.h"
#endif
#ifndef MEP_GOOGLE_OAUTH_CLIENT_ID
#define MEP_GOOGLE_OAUTH_CLIENT_ID ""
#define MEP_GOOGLE_OAUTH_CLIENT_SECRET ""
#define MEP_MICROSOFT_OAUTH_CLIENT_ID ""
#endif

#if defined(_WIN32)
#include "platform_compat.h"
#else
#include <sys/stat.h>
#endif

namespace mail {

// --- Small helpers ------------------------------------------------------

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool IStartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

void AppendUtf8(std::string *out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        AppendUtf8(out, 0xFFFD);
    }
}

// Decodes one code point at s[*i]; advances *i. Invalid sequences yield
// U+FFFD and consume one byte.
uint32_t NextCodePoint(std::string_view s, size_t *i) {
    const unsigned char c = static_cast<unsigned char>(s[*i]);
    size_t len = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
        (*i)++;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07u;
    } else {
        (*i)++;
        return 0xFFFD;
    }
    if (*i + len > s.size()) {
        (*i)++;
        return 0xFFFD;
    }
    for (size_t k = 1; k < len; k++) {
        const unsigned char cc = static_cast<unsigned char>(s[*i + k]);
        if ((cc & 0xC0) != 0x80) {
            (*i)++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        (*i)++;
        return 0xFFFD;
    }
    *i += len;
    return cp;
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t before = i;
        uint32_t cp = NextCodePoint(s, &i);
        if (cp == 0xFFFD && !(c == 0xEF && i - before == 3)) return false;
    }
    return true;
}

std::string SanitizeUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) AppendUtf8(&out, NextCodePoint(s, &i));
    return out;
}

// Windows-1252's 0x80..0x9F block (the rest is ISO-8859-1).
const uint16_t kCp1252High[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                  0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

std::string SingleByteToUtf8(std::string_view bytes, int table) {
    // table: 0 = ISO-8859-1, 1 = Windows-1252, 2 = ISO-8859-15
    std::string out;
    out.reserve(bytes.size());
    for (char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        uint32_t cp = c;
        if (table == 1 && c >= 0x80 && c < 0xA0) cp = kCp1252High[c - 0x80];
        if (table == 2) {
            switch (c) {
                case 0xA4: cp = 0x20AC; break;
                case 0xA6: cp = 0x0160; break;
                case 0xA8: cp = 0x0161; break;
                case 0xB4: cp = 0x017D; break;
                case 0xB8: cp = 0x017E; break;
                case 0xBC: cp = 0x0152; break;
                case 0xBD: cp = 0x0153; break;
                case 0xBE: cp = 0x0178; break;
                default: break;
            }
        }
        AppendUtf8(&out, cp);
    }
    return out;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

// --- Encodings ----------------------------------------------------------

std::string Base64Encode(std::string_view in, size_t wrap) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4 + (wrap ? in.size() / wrap * 2 + 2 : 0));
    size_t col = 0;
    auto put = [&](char c) {
        out.push_back(c);
        if (wrap && ++col == wrap) {
            out += "\r\n";
            col = 0;
        }
    };
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8) |
                           static_cast<uint32_t>(static_cast<unsigned char>(in[i + 2]));
        put(kB64[(v >> 18) & 63]);
        put(kB64[(v >> 12) & 63]);
        put(kB64[(v >> 6) & 63]);
        put(kB64[v & 63]);
    }
    if (i < in.size()) {
        uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(in[i])) << 16;
        if (i + 1 < in.size()) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8;
        put(kB64[(v >> 18) & 63]);
        put(kB64[(v >> 12) & 63]);
        put(i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=');
        put('=');
    }
    if (wrap && col != 0) out += "\r\n";
    return out;
}

std::string Base64Decode(std::string_view in) {
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string QpDecode(std::string_view in, bool header) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        const char c = in[i];
        if (c == '=') {
            // Soft line break: "=", optional whitespace, then (CR)LF or the end.
            size_t j = i + 1;
            while (j < in.size() && (in[j] == ' ' || in[j] == '\t')) j++;
            if (j >= in.size()) break;
            if (in[j] == '\r' || in[j] == '\n') {
                if (in[j] == '\r' && j + 1 < in.size() && in[j + 1] == '\n') j++;
                i = j;
                continue;
            }
            const int hi = i + 1 < in.size() ? HexVal(in[i + 1]) : -1;
            const int lo = i + 2 < in.size() ? HexVal(in[i + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
            out.push_back('=');
        } else if (header && c == '_') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string QpEncode(std::string_view in) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    size_t col = 0;
    auto emit = [&](const std::string &piece) {
        if (col + piece.size() > 75) {
            out += "=\r\n";
            col = 0;
        }
        out += piece;
        col += piece.size();
    };
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c == '\r' && i + 1 < in.size() && in[i + 1] == '\n') {
            i++;
            continue;
        }
        if (c == '\n') {
            out += "\r\n";
            col = 0;
            i++;
            continue;
        }
        const unsigned char u = static_cast<unsigned char>(c);
        const bool at_eol = i + 1 >= in.size() || in[i + 1] == '\n' || in[i + 1] == '\r';
        const bool literal = (u >= 33 && u <= 126 && u != '=') || ((u == ' ' || u == '\t') && !at_eol);
        // A leading '.' is quoted too: some relays still choke on a lone "." line.
        if (literal && !(c == '.' && col == 0)) {
            emit(std::string(1, c));
        } else {
            std::string esc = "=";
            esc.push_back(kHex[u >> 4]);
            esc.push_back(kHex[u & 15]);
            emit(esc);
        }
        i++;
    }
    return out;
}

std::string ToUtf8(std::string_view bytes, std::string_view charset_in) {
    std::string cs = Lower(Trim(charset_in));
    if (cs.size() >= 2 && cs.front() == '"' && cs.back() == '"') cs = cs.substr(1, cs.size() - 2);
    if (cs == "iso-8859-1" || cs == "latin1" || cs == "iso8859-1" || cs == "l1" || cs == "iso_8859-1") {
        return SingleByteToUtf8(bytes, 0);
    }
    if (cs == "windows-1252" || cs == "cp1252" || cs == "x-cp1252") return SingleByteToUtf8(bytes, 1);
    if (cs == "iso-8859-15" || cs == "latin9" || cs == "iso8859-15") return SingleByteToUtf8(bytes, 2);
    if (cs.empty() || cs == "utf-8" || cs == "utf8" || cs == "us-ascii" || cs == "ascii") {
        if (IsValidUtf8(bytes)) return std::string(bytes);
        // Mislabeled mail is common: 8-bit text claiming ASCII/UTF-8 is
        // nearly always Windows-1252.
        return cs.rfind("utf", 0) == 0 ? SanitizeUtf8(bytes) : SingleByteToUtf8(bytes, 1);
    }
    if (IsValidUtf8(bytes)) return std::string(bytes);
    return SingleByteToUtf8(bytes, 1);
}

std::string DecodeHeaderWords(std::string_view v) {
    std::string out;
    size_t i = 0;
    bool last_was_word = false;
    std::string pending_ws;
    while (i < v.size()) {
        if (v[i] == '=' && i + 1 < v.size() && v[i + 1] == '?') {
            // =?charset?enc?text?=
            const size_t q1 = v.find('?', i + 2);
            const size_t q2 = q1 == std::string_view::npos ? q1 : v.find('?', q1 + 1);
            const size_t end = q2 == std::string_view::npos ? q2 : v.find("?=", q2 + 1);
            if (end != std::string_view::npos && q2 == q1 + 2) {
                std::string charset(v.substr(i + 2, q1 - i - 2));
                const size_t star = charset.find('*');  // RFC 2231 language suffix
                if (star != std::string::npos) charset.resize(star);
                const char enc = static_cast<char>(std::toupper(static_cast<unsigned char>(v[q1 + 1])));
                std::string_view text = v.substr(q2 + 1, end - q2 - 1);
                std::string bytes;
                if (enc == 'B') bytes = Base64Decode(text);
                else if (enc == 'Q') bytes = QpDecode(text, true);
                else bytes = std::string(text);
                if (!last_was_word) out += pending_ws;
                pending_ws.clear();
                out += ToUtf8(bytes, charset);
                last_was_word = true;
                i = end + 2;
                continue;
            }
        }
        if (v[i] == ' ' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n') {
            pending_ws.push_back(v[i] == '\t' || v[i] == ' ' ? v[i] : ' ');
            i++;
            continue;
        }
        out += pending_ws;
        pending_ws.clear();
        last_was_word = false;
        // A run of ordinary text up to the next whitespace or encoded word.
        size_t j = i;
        while (j < v.size() && v[j] != ' ' && v[j] != '\t' && v[j] != '\r' && v[j] != '\n' &&
               !(v[j] == '=' && j + 1 < v.size() && v[j + 1] == '?' && j > i)) {
            j++;
        }
        if (j == i) j = i + 1;
        out += ToUtf8(v.substr(i, j - i), "utf-8");
        i = j;
    }
    out += pending_ws;
    return Trim(out);
}

std::string EncodeHeaderWords(std::string_view v) {
    bool plain = true;
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80 || (u < 0x20 && c != '\t')) plain = false;
    }
    if (plain && v.find("=?") == std::string_view::npos) return std::string(v);
    // Split on code point boundaries so each word decodes on its own:
    // "=?UTF-8?B?" + base64 + "?=" must stay within 75 chars, which is 45
    // raw bytes of base64 input.
    std::string out;
    size_t i = 0;
    while (i < v.size()) {
        size_t j = i;
        while (j < v.size()) {
            size_t k = j;
            NextCodePoint(v, &k);
            if (k - i > 45) break;
            j = k;
        }
        if (j == i) j = i + 1;
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + Base64Encode(v.substr(i, j - i)) + "?=";
        i = j;
    }
    return out;
}

std::string DecodeMailboxName(std::string_view s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        const size_t end = s.find('-', i + 1);
        if (end == std::string_view::npos) {
            out += std::string(s.substr(i));
            break;
        }
        if (end == i + 1) {
            out.push_back('&');
            i = end + 1;
            continue;
        }
        std::string b64(s.substr(i + 1, end - i - 1));
        for (char &c : b64) {
            if (c == ',') c = '/';
        }
        const std::string utf16 = Base64Decode(b64);
        for (size_t k = 0; k + 1 < utf16.size(); k += 2) {
            uint32_t u = (static_cast<uint32_t>(static_cast<unsigned char>(utf16[k])) << 8) |
                         static_cast<uint32_t>(static_cast<unsigned char>(utf16[k + 1]));
            if (u >= 0xD800 && u <= 0xDBFF && k + 3 < utf16.size()) {
                const uint32_t lo = (static_cast<uint32_t>(static_cast<unsigned char>(utf16[k + 2])) << 8) |
                                    static_cast<uint32_t>(static_cast<unsigned char>(utf16[k + 3]));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    k += 2;
                }
            }
            AppendUtf8(&out, u);
        }
        i = end + 1;
    }
    return out;
}

std::string EncodeMailboxName(std::string_view s) {
    std::string out;
    std::string utf16;
    auto flush = [&]() {
        if (utf16.empty()) return;
        std::string b = Base64Encode(utf16);
        while (!b.empty() && b.back() == '=') b.pop_back();
        for (char &c : b) {
            if (c == '/') c = ',';
        }
        out += "&" + b + "-";
        utf16.clear();
    };
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F) {
            flush();
            out += c == '&' ? std::string("&-") : std::string(1, static_cast<char>(c));
            i++;
            continue;
        }
        uint32_t cp = NextCodePoint(s, &i);
        auto put16 = [&](uint32_t u) {
            utf16.push_back(static_cast<char>((u >> 8) & 0xFF));
            utf16.push_back(static_cast<char>(u & 0xFF));
        };
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put16(0xD800 + (cp >> 10));
            put16(0xDC00 + (cp & 0x3FF));
        } else {
            put16(cp);
        }
    }
    flush();
    return out;
}

// --- RFC 5322 -----------------------------------------------------------

std::vector<Header> ParseHeaders(std::string_view block) {
    std::vector<Header> out;
    size_t pos = 0;
    while (pos < block.size()) {
        size_t eol = block.find('\n', pos);
        if (eol == std::string_view::npos) eol = block.size();
        std::string_view line = block.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = eol + 1;
        if (line.empty()) break;
        if ((line[0] == ' ' || line[0] == '\t')) {
            if (!out.empty()) {
                out.back().value += ' ';
                out.back().value += Trim(line);
            }
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        Header h;
        h.name = Trim(line.substr(0, colon));
        h.value = Trim(line.substr(colon + 1));
        out.push_back(std::move(h));
    }
    return out;
}

std::string HeaderValue(const std::vector<Header> &headers, std::string_view name) {
    for (const Header &h : headers) {
        if (IEquals(h.name, name)) return h.value;
    }
    return "";
}

std::string Address::Display() const {
    if (name.empty()) return email;
    if (email.empty()) return name;
    return name + " <" + email + ">";
}

std::string Address::Short() const { return name.empty() ? email : name; }

std::vector<Address> ParseAddressList(std::string_view v) {
    std::vector<Address> out;
    // Split on commas outside quotes, angle brackets and comments; a group
    // ("team: a@x, b@y;") contributes its members.
    std::vector<std::string> parts;
    std::string cur;
    bool in_quote = false;
    int angle = 0, paren = 0;
    for (size_t i = 0; i < v.size(); i++) {
        const char c = v[i];
        if (in_quote) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < v.size()) {
                cur.push_back(v[++i]);
            } else if (c == '"') {
                in_quote = false;
            }
            continue;
        }
        if (c == '"') in_quote = true;
        else if (c == '<') angle++;
        else if (c == '>' && angle > 0) angle--;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        if ((c == ',' || c == ';') && angle == 0 && paren == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == ':' && angle == 0 && paren == 0) {
            cur.clear();  // group name -- the members follow
            continue;
        }
        cur.push_back(c);
    }
    parts.push_back(cur);
    for (const std::string &raw : parts) {
        std::string p = Trim(raw);
        if (p.empty()) continue;
        Address a;
        const size_t lt = p.rfind('<');
        const size_t gt = p.rfind('>');
        std::string name_part;
        if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
            a.email = Trim(p.substr(lt + 1, gt - lt - 1));
            name_part = Trim(p.substr(0, lt));
        } else {
            // bare addr-spec, maybe with a "(Name)" comment
            const size_t lp = p.find('(');
            const size_t rp = p.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
                name_part = p.substr(lp + 1, rp - lp - 1);
                a.email = Trim(p.substr(0, lp) + p.substr(rp + 1));
            } else {
                a.email = p;
            }
        }
        // Strip quotes (unescaping), then decode encoded words.
        std::string unq;
        bool q = false;
        for (size_t i = 0; i < name_part.size(); i++) {
            const char c = name_part[i];
            if (c == '"') {
                q = !q;
                continue;
            }
            if (q && c == '\\' && i + 1 < name_part.size()) {
                unq.push_back(name_part[++i]);
                continue;
            }
            unq.push_back(c);
        }
        a.name = DecodeHeaderWords(unq);
        if (a.email.find('@') == std::string::npos && a.name.empty() && a.email.find(' ') != std::string::npos) {
            continue;  // not an address at all
        }
        if (a.name == a.email) a.name.clear();
        if (!a.email.empty() || !a.name.empty()) out.push_back(std::move(a));
    }
    return out;
}

std::string FormatAddressList(const std::vector<Address> &addrs) {
    std::string out;
    for (const Address &a : addrs) {
        if (!out.empty()) out += ", ";
        if (a.name.empty()) {
            out += a.email;
            continue;
        }
        bool ascii = true, needs_quote = false;
        for (char c : a.name) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 0x80) ascii = false;
            if (std::strchr("()<>[]:;@\\,.\"", c)) needs_quote = true;
        }
        if (!ascii) {
            out += EncodeHeaderWords(a.name);
        } else if (needs_quote) {
            std::string q = "\"";
            for (char c : a.name) {
                if (c == '"' || c == '\\') q.push_back('\\');
                q.push_back(c);
            }
            out += q + "\"";
        } else {
            out += a.name;
        }
        out += " <" + a.email + ">";
    }
    return out;
}

namespace {

int MonthIndex(std::string_view m) {
    static const char *kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (m.size() < 3) return -1;
    const std::string l = Lower(m.substr(0, 3));
    for (int i = 0; i < 12; i++) {
        if (l == kMonths[i]) return i;
    }
    return -1;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
long long DaysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool LocalTm(long long t, std::tm *out) {
    const std::time_t tt = static_cast<std::time_t>(t);
#if defined(_WIN32)
    return localtime_s(out, &tt) == 0;
#else
    return localtime_r(&tt, out) != nullptr;
#endif
}

}  // namespace

long long ParseDate(std::string_view value) {
    std::string s(value);
    const size_t paren = s.find('(');  // trailing "(PDT)" comment
    if (paren != std::string::npos) s.resize(paren);
    // Tokens split on spaces and commas; an INTERNALDATE's "10-Oct-2026"
    // splits further on its dashes (a zone's leading '-' does not).
    std::vector<std::string> tok;
    std::string cur;
    auto push = [&]() {
        if (cur.empty()) return;
        const size_t dash = cur.find('-');
        if (dash != std::string::npos && dash > 0 && std::isdigit(static_cast<unsigned char>(cur[0])) != 0) {
            size_t p = 0;
            while (p <= cur.size()) {
                size_t e = cur.find('-', p);
                if (e == std::string::npos) e = cur.size();
                if (e > p) tok.push_back(cur.substr(p, e - p));
                p = e + 1;
            }
        } else {
            tok.push_back(cur);
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') push();
        else cur.push_back(c);
    }
    push();
    int day = -1, month = -1, hh = 0, mm = 0, ss = 0;
    long long year = -1;
    long zone_sec = 0;
    bool have_time = false;
    for (const std::string &t : tok) {
        if (t.empty()) continue;
        if (t.find(':') != std::string::npos && !have_time) {
            int a = 0, b = 0, c = 0;
            const int n = std::sscanf(t.c_str(), "%d:%d:%d", &a, &b, &c);
            if (n >= 2) {
                hh = a;
                mm = b;
                ss = n == 3 ? c : 0;
                have_time = true;
            }
            continue;
        }
        if ((t[0] == '+' || t[0] == '-') && t.size() == 5 && std::isdigit(static_cast<unsigned char>(t[1]))) {
            const int v = std::atoi(t.c_str() + 1);
            zone_sec = (v / 100) * 3600L + (v % 100) * 60L;
            if (t[0] == '-') zone_sec = -zone_sec;
            continue;
        }
        if (month < 0 && MonthIndex(t) >= 0 && !std::isdigit(static_cast<unsigned char>(t[0]))) {
            month = MonthIndex(t);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(t[0]))) {
            const long long v = std::atoll(t.c_str());
            if (day < 0 && v >= 1 && v <= 31 && t.size() <= 2) day = static_cast<int>(v);
            else if (year < 0) year = v;
            continue;
        }
        const std::string z = Lower(t);
        if (z == "ut" || z == "utc" || z == "gmt" || z == "z") zone_sec = 0;
        else if (z == "edt") zone_sec = -4 * 3600;
        else if (z == "est" || z == "cdt") zone_sec = -5 * 3600;
        else if (z == "cst" || z == "mdt") zone_sec = -6 * 3600;
        else if (z == "mst" || z == "pdt") zone_sec = -7 * 3600;
        else if (z == "pst") zone_sec = -8 * 3600;
    }
    if (day < 0 || month < 0 || year < 0) return -1;
    if (year < 50) year += 2000;
    else if (year < 1000) year += 1900;
    const long long days = DaysFromCivil(year, static_cast<unsigned>(month + 1), static_cast<unsigned>(day));
    return days * 86400 + hh * 3600LL + mm * 60LL + ss - zone_sec;
}

std::string FormatRfc5322Date(long long t) {
    std::tm tm{};
    if (!LocalTm(t, &tm)) return "";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S ", &tm);
    // The offset from the local broken-down time (portable: no %z on all CRTs).
    const long long local_as_utc =
        DaysFromCivil(tm.tm_year + 1900LL, static_cast<unsigned>(tm.tm_mon + 1), static_cast<unsigned>(tm.tm_mday)) *
            86400 +
        tm.tm_hour * 3600LL + tm.tm_min * 60LL + tm.tm_sec;
    long long off = (local_as_utc - t) / 60;
    char zone[16];
    const char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    std::snprintf(zone, sizeof(zone), "%c%02lld%02lld", sign, off / 60, off % 60);
    return std::string(buf) + zone;
}

std::string FormatListDate(long long t, long long now) {
    if (t < 0) return "";
    std::tm tm{}, tn{};
    if (!LocalTm(t, &tm) || !LocalTm(now, &tn)) return "";
    char buf[32];
    if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday) {
        std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    } else if (tm.tm_year == tn.tm_year) {
        std::strftime(buf, sizeof(buf), "%b %d", &tm);
    } else {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    }
    return buf;
}

std::string FormatLongDate(long long t) {
    if (t < 0) return "";
    std::tm tm{};
    if (!LocalTm(t, &tm)) return "";
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M", &tm);
    return buf;
}

// --- MIME ---------------------------------------------------------------

namespace {

// "text/plain; charset=utf-8; format=flowed" -> type + params (lower-cased names).
struct ContentType {
    std::string type = "text/plain";
    std::vector<std::pair<std::string, std::string>> params;
    std::string Param(std::string_view name) const {
        for (const auto &p : params) {
            if (p.first == name) return p.second;
        }
        return "";
    }
};

ContentType ParseContentType(std::string_view v) {
    ContentType ct;
    std::vector<std::string> pieces;
    std::string cur;
    bool q = false;
    for (char c : v) {
        if (c == '"') q = !q;
        if (c == ';' && !q) {
            pieces.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    pieces.push_back(cur);
    if (!pieces.empty() && !Trim(pieces[0]).empty()) ct.type = Lower(Trim(pieces[0]));
    // RFC 2231 continuations (name*0*=..., name*1=...) are joined here.
    std::vector<std::pair<std::string, std::string>> raw;
    for (size_t i = 1; i < pieces.size(); i++) {
        const size_t eq = pieces[i].find('=');
        if (eq == std::string::npos) continue;
        std::string name = Lower(Trim(pieces[i].substr(0, eq)));
        std::string val = Trim(pieces[i].substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            std::string unq;
            for (size_t k = 1; k + 1 < val.size(); k++) {
                if (val[k] == '\\' && k + 2 < val.size()) k++;
                unq.push_back(val[k]);
            }
            val = unq;
        }
        raw.emplace_back(name, val);
    }
    for (const auto &p : raw) {
        const size_t star = p.first.find('*');
        if (star == std::string::npos) {
            ct.params.emplace_back(p.first, p.second);
            continue;
        }
        const std::string base = p.first.substr(0, star);
        const bool encoded = p.first.back() == '*';
        std::string val = p.second;
        if (encoded) {
            // charset'lang'percent-encoded (only on the first segment), then %XX.
            std::string charset;
            const size_t a1 = val.find('\'');
            const size_t a2 = a1 == std::string::npos ? a1 : val.find('\'', a1 + 1);
            if (a2 != std::string::npos && (p.first.find("*0") != std::string::npos || p.first == base + "*")) {
                charset = val.substr(0, a1);
                val = val.substr(a2 + 1);
            }
            std::string dec;
            for (size_t k = 0; k < val.size(); k++) {
                if (val[k] == '%' && k + 2 < val.size() && HexVal(val[k + 1]) >= 0 && HexVal(val[k + 2]) >= 0) {
                    dec.push_back(static_cast<char>(HexVal(val[k + 1]) * 16 + HexVal(val[k + 2])));
                    k += 2;
                } else {
                    dec.push_back(val[k]);
                }
            }
            val = ToUtf8(dec, charset.empty() ? "utf-8" : charset);
        }
        bool found = false;
        for (auto &q2 : ct.params) {
            if (q2.first == base) {
                q2.second += val;
                found = true;
            }
        }
        if (!found) ct.params.emplace_back(base, val);
    }
    return ct;
}

std::string DecodeTransfer(std::string_view body, std::string_view cte) {
    const std::string e = Lower(Trim(cte));
    if (e == "base64") return Base64Decode(body);
    if (e == "quoted-printable") return QpDecode(body);
    return std::string(body);
}

// Splits a message/part into its header block and body.
void SplitHeadBody(std::string_view raw, std::string_view *head, std::string_view *body) {
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string_view::npos) {
            *head = raw;
            *body = std::string_view();
            return;
        }
        std::string_view line = raw.substr(pos, eol - pos);
        if (line.empty() || line == "\r") {
            *head = raw.substr(0, pos);
            *body = raw.substr(eol + 1);
            return;
        }
        pos = eol + 1;
    }
    *head = raw;
    *body = std::string_view();
}

std::vector<std::string_view> SplitMultipart(std::string_view body, const std::string &boundary) {
    std::vector<std::string_view> parts;
    if (boundary.empty()) return parts;
    const std::string delim = "--" + boundary;
    size_t pos = 0;
    long long part_start = -1;
    while (pos <= body.size()) {
        size_t eol = body.find('\n', pos);
        const size_t line_end = eol == std::string_view::npos ? body.size() : eol;
        std::string_view line = body.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() >= delim.size() && line.substr(0, delim.size()) == delim) {
            std::string_view rest = line.substr(delim.size());
            while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.remove_suffix(1);
            const bool closing = rest == "--";
            if (rest.empty() || closing) {
                if (part_start >= 0) {
                    // The CRLF before the delimiter belongs to the delimiter.
                    size_t end = pos;
                    if (end > 0 && body[end - 1] == '\n') end--;
                    if (end > 0 && body[end - 1] == '\r') end--;
                    const size_t start = static_cast<size_t>(part_start);
                    parts.push_back(body.substr(start, end >= start ? end - start : 0));
                }
                if (closing) return parts;
                part_start = eol == std::string_view::npos ? static_cast<long long>(body.size())
                                                            : static_cast<long long>(eol + 1);
            }
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    if (part_start >= 0 && static_cast<size_t>(part_start) < body.size()) {
        parts.push_back(body.substr(static_cast<size_t>(part_start)));
    }
    return parts;
}

struct WalkState {
    std::string plain;
    std::string html;
    std::vector<Attachment> attachments;
};

std::string FilenameOf(const std::vector<Header> &hs, const ContentType &ct) {
    const ContentType disp = ParseContentType(HeaderValue(hs, "Content-Disposition"));
    std::string fn = disp.Param("filename");
    if (fn.empty()) fn = ct.Param("name");
    return DecodeHeaderWords(fn);
}

void WalkPart(std::string_view raw, WalkState *st, int depth, const std::string &default_type) {
    if (depth > 20) return;
    std::string_view head, body;
    SplitHeadBody(raw, &head, &body);
    const std::vector<Header> hs = ParseHeaders(head);
    std::string ct_raw = HeaderValue(hs, "Content-Type");
    ContentType ct = ParseContentType(ct_raw.empty() ? default_type : ct_raw);
    const std::string disp = Lower(HeaderValue(hs, "Content-Disposition"));
    const bool is_attachment = disp.rfind("attachment", 0) == 0;
    if (ct.type.rfind("multipart/", 0) == 0) {
        const std::string sub_default = ct.type == "multipart/digest" ? "message/rfc822" : "text/plain";
        std::vector<std::string_view> parts = SplitMultipart(body, ct.Param("boundary"));
        if (ct.type == "multipart/alternative") {
            // Prefer the last plain-text alternative for text, keep the last HTML too.
            for (std::string_view p : parts) WalkPart(p, st, depth + 1, sub_default);
            return;
        }
        for (std::string_view p : parts) WalkPart(p, st, depth + 1, sub_default);
        return;
    }
    const std::string data = DecodeTransfer(body, HeaderValue(hs, "Content-Transfer-Encoding"));
    std::string filename = FilenameOf(hs, ct);
    if (!is_attachment && filename.empty() && (ct.type == "text/plain" || ct.type == "text/html")) {
        std::string text = ToUtf8(data, ct.Param("charset"));
        std::string &dst = ct.type == "text/plain" ? st->plain : st->html;
        if (!dst.empty()) dst += ct.type == "text/plain" ? "\n\n" : "<hr>";
        dst += text;
        return;
    }
    if (ct.type == "message/rfc822" && !is_attachment) {
        // An inline forwarded message: show it as text, keep its own attachments.
        WalkState inner;
        WalkPart(data, &inner, depth + 1, "text/plain");
        const Message fwd = ParseMessage(data);
        std::string block = "\n---------- Forwarded message ----------\n";
        if (!fwd.from.empty()) block += "From: " + fwd.from[0].Display() + "\n";
        if (!fwd.date_raw.empty()) block += "Date: " + fwd.date_raw + "\n";
        block += "Subject: " + fwd.subject + "\n\n" + fwd.text + "\n";
        st->plain += block;
        for (Attachment &a : inner.attachments) st->attachments.push_back(std::move(a));
        return;
    }
    Attachment a;
    a.content_type = ct.type;
    a.filename = filename;
    if (a.filename.empty()) {
        if (ct.type == "message/rfc822") a.filename = "message.eml";
        else if (ct.type == "text/calendar") a.filename = "invite.ics";
        else a.filename = "part" + std::to_string(st->attachments.size() + 1);
    }
    a.data = data;
    a.content_id = Trim(HeaderValue(hs, "Content-ID"));
    if (a.content_id.size() >= 2 && a.content_id.front() == '<' && a.content_id.back() == '>') {
        a.content_id = a.content_id.substr(1, a.content_id.size() - 2);
    }
    st->attachments.push_back(std::move(a));
}

// Text with trailing whitespace stripped per line and runs of blank lines squeezed to one.
std::string TidyText(const std::string &s) {
    std::string out;
    std::istringstream in(s);
    std::string line;
    int blanks = 0;
    bool any = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) {
            blanks++;
            continue;
        }
        if (any) out += blanks > 0 ? "\n\n" : "\n";
        out += line;
        blanks = 0;
        any = true;
    }
    return out;
}

}  // namespace

Message ParseMessage(std::string_view raw) {
    Message m;
    std::string_view head, body;
    SplitHeadBody(raw, &head, &body);
    m.headers = ParseHeaders(head);
    m.subject = DecodeHeaderWords(HeaderValue(m.headers, "Subject"));
    m.from = ParseAddressList(HeaderValue(m.headers, "From"));
    m.to = ParseAddressList(HeaderValue(m.headers, "To"));
    m.cc = ParseAddressList(HeaderValue(m.headers, "Cc"));
    m.reply_to = ParseAddressList(HeaderValue(m.headers, "Reply-To"));
    m.date_raw = HeaderValue(m.headers, "Date");
    m.date = ParseDate(m.date_raw);
    m.message_id = Trim(HeaderValue(m.headers, "Message-ID"));
    m.in_reply_to = Trim(HeaderValue(m.headers, "In-Reply-To"));
    m.references = Trim(HeaderValue(m.headers, "References"));
    WalkState st;
    WalkPart(raw, &st, 0, "text/plain");
    m.html = st.html;
    if (!Trim(st.plain).empty()) {
        m.text = TidyText(st.plain);
    } else if (!st.html.empty()) {
        m.text = HtmlToText(st.html);
        m.text_from_html = true;
    }
    m.attachments = std::move(st.attachments);
    return m;
}

namespace {

struct Entity {
    const char *name;
    uint32_t cp;
};

const Entity kEntities[] = {
    {"amp", '&'},       {"lt", '<'},         {"gt", '>'},         {"quot", '"'},       {"apos", '\''},
    {"nbsp", 0xA0},     {"copy", 0xA9},      {"reg", 0xAE},       {"trade", 0x2122},   {"mdash", 0x2014},
    {"ndash", 0x2013},  {"hellip", 0x2026},  {"lsquo", 0x2018},   {"rsquo", 0x2019},   {"ldquo", 0x201C},
    {"rdquo", 0x201D},  {"bull", 0x2022},    {"middot", 0xB7},    {"laquo", 0xAB},     {"raquo", 0xBB},
    {"euro", 0x20AC},   {"pound", 0xA3},     {"yen", 0xA5},       {"cent", 0xA2},      {"deg", 0xB0},
    {"times", 0xD7},    {"divide", 0xF7},    {"zwnj", 0x200C},    {"zwj", 0x200D},     {"shy", 0xAD},
    {"rarr", 0x2192},   {"larr", 0x2190},    {"eacute", 0xE9},    {"egrave", 0xE8},    {"aacute", 0xE1},
    {"agrave", 0xE0},   {"ouml", 0xF6},      {"uuml", 0xFC},      {"auml", 0xE4},      {"szlig", 0xDF},
    {"ccedil", 0xE7},   {"ntilde", 0xF1},    {"iacute", 0xED},    {"oacute", 0xF3},    {"uacute", 0xFA},
    {"thinsp", 0x2009}, {"ensp", 0x2002},    {"emsp", 0x2003},    {"sect", 0xA7},      {"para", 0xB6},
};

std::string DecodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (!ent.empty() && ent[0] == '#') {
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                cp = static_cast<uint32_t>(std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16));
            } else {
                cp = static_cast<uint32_t>(std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10));
            }
        } else {
            for (const Entity &e : kEntities) {
                if (ent == e.name) cp = e.cp;
            }
        }
        if (cp == 0) {
            out.push_back('&');
            continue;
        }
        if (cp == 0xA0) cp = ' ';
        if (cp == 0x200C || cp == 0x200D || cp == 0xAD || cp == 0x34F) {
            i = semi;
            continue;  // invisible; marketing mail pads previews with these
        }
        AppendUtf8(&out, cp);
        i = semi;
    }
    return out;
}

}  // namespace

std::string HtmlToText(std::string_view html) {
    std::string out;
    std::vector<std::string> links;
    std::string pending_href;
    size_t link_text_start = 0;
    int skip_depth = 0;  // inside <style>/<script>/<head>/<title>
    int pre_depth = 0;
    int list_depth = 0;
    bool space_pending = false;
    auto newline = [&](int n) {
        // Ensure the output ends with at least n newlines (n=1 line break, 2 paragraph).
        space_pending = false;
        if (out.empty()) return;
        int have = 0;
        for (size_t k = out.size(); k > 0 && out[k - 1] == '\n'; k--) have++;
        while (have < n) {
            out.push_back('\n');
            have++;
        }
    };
    auto put_text = [&](std::string_view t) {
        if (skip_depth > 0) return;
        const std::string dec = DecodeEntities(t);
        if (pre_depth > 0) {
            out += dec;
            return;
        }
        for (char c : dec) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                space_pending = true;
                continue;
            }
            if (space_pending && !out.empty() && out.back() != '\n' && out.back() != ' ') out.push_back(' ');
            space_pending = false;
            out.push_back(c);
        }
    };
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            const size_t lt = html.find('<', i);
            const size_t end = lt == std::string_view::npos ? html.size() : lt;
            put_text(html.substr(i, end - i));
            i = end;
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            const size_t e = html.find("-->", i + 4);
            i = e == std::string_view::npos ? html.size() : e + 3;
            continue;
        }
        // Find the tag's end, respecting quoted attribute values.
        size_t j = i + 1;
        char q = 0;
        while (j < html.size()) {
            if (q) {
                if (html[j] == q) q = 0;
            } else if (html[j] == '"' || html[j] == '\'') {
                q = html[j];
            } else if (html[j] == '>') {
                break;
            }
            j++;
        }
        std::string_view tag = html.substr(i + 1, (j < html.size() ? j : html.size()) - i - 1);
        i = j < html.size() ? j + 1 : html.size();
        bool closing = !tag.empty() && tag[0] == '/';
        if (closing) tag.remove_prefix(1);
        size_t ne = 0;
        while (ne < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[ne])) != 0)) ne++;
        const std::string name = Lower(tag.substr(0, ne));
        if (name.empty()) continue;
        if (name == "style" || name == "script" || name == "head" || name == "title") {
            if (closing) skip_depth = std::max(0, skip_depth - 1);
            else if (tag.empty() || tag.back() != '/') skip_depth++;
            continue;
        }
        if (skip_depth > 0) continue;
        if (name == "br") {
            out.push_back('\n');
            space_pending = false;
        } else if (name == "p" || name == "div" || name == "table" || name == "blockquote" || name == "section" ||
                   name == "article" || name == "header" || name == "footer" || name == "center") {
            newline(name == "p" || name == "blockquote" || name == "table" ? 2 : 1);
        } else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            newline(2);
        } else if (name == "tr") {
            newline(1);
        } else if (name == "td" || name == "th") {
            if (!closing) space_pending = true;
        } else if (name == "hr") {
            newline(1);
            out += "----------------------------------------";
            newline(1);
        } else if (name == "ul" || name == "ol") {
            list_depth = closing ? std::max(0, list_depth - 1) : list_depth + 1;
            newline(1);
        } else if (name == "li") {
            if (!closing) {
                newline(1);
                out += std::string(static_cast<size_t>(std::max(0, list_depth - 1)) * 2, ' ') + "- ";
            }
        } else if (name == "pre") {
            pre_depth = closing ? std::max(0, pre_depth - 1) : pre_depth + 1;
            newline(1);
        } else if (name == "img" && !closing) {
            // alt text stands in for the picture
            const std::string lt = Lower(tag);
            const size_t a = lt.find("alt=");
            if (a != std::string::npos && a + 4 < tag.size()) {
                const char qc = tag[a + 4];
                if (qc == '"' || qc == '\'') {
                    const size_t e = tag.find(qc, a + 5);
                    if (e != std::string_view::npos && e > a + 5) put_text(tag.substr(a + 5, e - a - 5));
                }
            }
        } else if (name == "a") {
            if (!closing) {
                pending_href.clear();
                const std::string lt = Lower(tag);
                const size_t h = lt.find("href=");
                if (h != std::string::npos && h + 5 < tag.size()) {
                    const char qc = tag[h + 5];
                    if (qc == '"' || qc == '\'') {
                        const size_t e = tag.find(qc, h + 6);
                        if (e != std::string_view::npos) pending_href = DecodeEntities(tag.substr(h + 6, e - h - 6));
                    } else {
                        size_t e = h + 5;
                        while (e < tag.size() && tag[e] != ' ') e++;
                        pending_href = DecodeEntities(tag.substr(h + 5, e - h - 5));
                    }
                }
                link_text_start = out.size();
            } else if (!pending_href.empty()) {
                const std::string text = Trim(std::string_view(out).substr(std::min(link_text_start, out.size())));
                const bool is_web = pending_href.rfind("http", 0) == 0 || pending_href.rfind("mailto:", 0) == 0;
                if (is_web && text != pending_href) {
                    size_t idx = 0;
                    for (; idx < links.size(); idx++) {
                        if (links[idx] == pending_href) break;
                    }
                    if (idx == links.size()) links.push_back(pending_href);
                    out += " [" + std::to_string(idx + 1) + "]";
                }
                pending_href.clear();
            }
        }
    }
    std::string text = TidyText(out);
    if (!links.empty()) {
        text += "\n\nLinks:\n";
        for (size_t k = 0; k < links.size(); k++) text += "[" + std::to_string(k + 1) + "] " + links[k] + "\n";
        if (!text.empty() && text.back() == '\n') text.pop_back();
    }
    return text;
}

std::string GuessContentType(std::string_view filename) {
    const size_t dot = filename.rfind('.');
    const std::string ext = dot == std::string_view::npos ? "" : Lower(filename.substr(dot + 1));
    static const std::pair<const char *, const char *> kTypes[] = {
        {"pdf", "application/pdf"},  {"png", "image/png"},        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},      {"gif", "image/gif"},        {"webp", "image/webp"},
        {"svg", "image/svg+xml"},    {"txt", "text/plain"},       {"md", "text/markdown"},
        {"org", "text/plain"},       {"csv", "text/csv"},         {"html", "text/html"},
        {"htm", "text/html"},        {"ics", "text/calendar"},    {"json", "application/json"},
        {"xml", "application/xml"},  {"zip", "application/zip"},  {"gz", "application/gzip"},
        {"tar", "application/x-tar"}, {"mp3", "audio/mpeg"},      {"mp4", "video/mp4"},
        {"wav", "audio/wav"},        {"eml", "message/rfc822"},   {"patch", "text/x-diff"},
        {"diff", "text/x-diff"},     {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"},
    };
    for (const auto &t : kTypes) {
        if (ext == t.first) return t.second;
    }
    return "application/octet-stream";
}

// --- Composing ----------------------------------------------------------

Draft ParseDraft(const std::vector<std::string> &lines) {
    Draft d;
    size_t i = 0;
    std::string *last = nullptr;
    for (; i < lines.size(); i++) {
        std::string line = lines[i];
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (Trim(line).empty()) {
            i++;
            break;
        }
        if ((line[0] == ' ' || line[0] == '\t') && last) {
            *last += " " + Trim(line);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) break;  // no blank line: the body starts here
        const std::string name = Lower(Trim(line.substr(0, colon)));
        const std::string value = Trim(line.substr(colon + 1));
        last = nullptr;
        if (name == "from") last = &d.from;
        else if (name == "to") last = &d.to;
        else if (name == "cc") last = &d.cc;
        else if (name == "bcc") last = &d.bcc;
        else if (name == "subject") last = &d.subject;
        else if (name == "in-reply-to") last = &d.in_reply_to;
        else if (name == "references") last = &d.references;
        else if (name == "attach" || name == "attachment") {
            if (!value.empty()) d.attach.push_back(value);
            continue;
        } else {
            continue;
        }
        *last = value;
    }
    std::string body;
    for (size_t k = i; k < lines.size(); k++) {
        body += lines[k];
        if (k + 1 < lines.size()) body += "\n";
    }
    d.body = body;
    return d;
}

std::vector<std::string> DraftLines(const Draft &d) {
    std::vector<std::string> out;
    out.push_back("From: " + d.from);
    out.push_back("To: " + d.to);
    out.push_back("Cc: " + d.cc);
    out.push_back("Bcc: " + d.bcc);
    out.push_back("Subject: " + d.subject);
    if (!d.in_reply_to.empty()) out.push_back("In-Reply-To: " + d.in_reply_to);
    if (!d.references.empty()) out.push_back("References: " + d.references);
    for (const std::string &a : d.attach) out.push_back("Attach: " + a);
    out.push_back("");
    std::istringstream in(d.body);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        out.push_back(line);
        any = true;
    }
    if (!any) out.push_back("");
    return out;
}

std::string ReplySubject(std::string_view subject) {
    const std::string s = Trim(subject);
    if (IStartsWith(s, "re:") || IStartsWith(s, "re[") || IStartsWith(s, "aw:")) return s;
    return "Re: " + s;
}

std::string ForwardSubject(std::string_view subject) {
    const std::string s = Trim(subject);
    if (IStartsWith(s, "fwd:") || IStartsWith(s, "fw:")) return s;
    return "Fwd: " + s;
}

namespace {

std::string QuoteBody(const std::string &text) {
    std::string out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out += line.empty() || line[0] == '>' ? ">" + line : "> " + line;
        out += "\n";
    }
    return out;
}

}  // namespace

Draft MakeReplyDraft(const Message &m, const std::string &from_line, const std::string &self_email, bool all) {
    Draft d;
    d.from = from_line;
    const std::vector<Address> &primary = m.reply_to.empty() ? m.from : m.reply_to;
    const std::string self = Lower(self_email);
    std::vector<Address> to, cc;
    std::set<std::string> seen;
    auto add = [&](std::vector<Address> *dst, const Address &a) {
        const std::string e = Lower(a.email);
        if (e.empty() || seen.count(e)) return;
        if (e == self && (all || dst == &cc)) return;
        seen.insert(e);
        dst->push_back(a);
    };
    for (const Address &a : primary) add(&to, a);
    if (all) {
        for (const Address &a : m.to) add(&to, a);
        for (const Address &a : m.cc) add(&cc, a);
        // Replying to one's own sent message: reply to its recipients instead.
        if (to.empty()) {
            for (const Address &a : m.to) {
                if (Lower(a.email) != self) to.push_back(a);
            }
        }
    }
    if (to.empty() && !primary.empty()) to = primary;
    d.to = FormatAddressList(to);
    d.cc = FormatAddressList(cc);
    d.subject = ReplySubject(m.subject);
    d.in_reply_to = m.message_id;
    if (!m.message_id.empty()) {
        d.references = Trim(m.references.empty() ? m.message_id : m.references + " " + m.message_id);
    }
    const std::string who = m.from.empty() ? std::string("someone") : m.from[0].Display();
    const std::string when = m.date >= 0 ? FormatLongDate(m.date) : m.date_raw;
    d.body = "\n\nOn " + when + ", " + who + " wrote:\n" + QuoteBody(m.text);
    return d;
}

Draft MakeForwardDraft(const Message &m, const std::string &from_line) {
    Draft d;
    d.from = from_line;
    d.subject = ForwardSubject(m.subject);
    std::string body = "\n\n---------- Forwarded message ----------\n";
    if (!m.from.empty()) body += "From: " + FormatAddressList(m.from) + "\n";
    if (!m.date_raw.empty()) body += "Date: " + m.date_raw + "\n";
    body += "Subject: " + m.subject + "\n";
    if (!m.to.empty()) body += "To: " + FormatAddressList(m.to) + "\n";
    if (!m.cc.empty()) body += "Cc: " + FormatAddressList(m.cc) + "\n";
    body += "\n" + m.text + "\n";
    d.body = body;
    return d;
}

std::vector<std::string> DraftRecipients(const Draft &d) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string *field : {&d.to, &d.cc, &d.bcc}) {
        for (const Address &a : ParseAddressList(*field)) {
            const std::string e = Lower(a.email);
            if (a.email.find('@') == std::string::npos || seen.count(e)) continue;
            seen.insert(e);
            out.push_back(a.email);
        }
    }
    return out;
}

std::string NewMessageId(const std::string &from_email, long long now) {
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(now));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llx.%016llx", static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(gen()));
    const size_t at = from_email.rfind('@');
    const std::string domain = at == std::string::npos ? "localhost" : from_email.substr(at + 1);
    return "<" + std::string(buf) + "@" + domain + ">";
}

namespace {

// Folds a header line at spaces so no line passes 78 chars where avoidable.
std::string FoldHeader(const std::string &name, const std::string &value) {
    std::string line = name + ": ";
    std::string out;
    size_t col = line.size();
    out = line;
    size_t i = 0;
    while (i < value.size()) {
        size_t sp = value.find(' ', i + 1);
        if (sp == std::string::npos) sp = value.size();
        std::string word = value.substr(i, sp - i);
        if (col + word.size() > 78 && col > name.size() + 2 && !word.empty() && word[0] == ' ') {
            out += "\r\n";
            col = 0;
        }
        out += word;
        col += word.size();
        const size_t nl = word.rfind('\n');
        if (nl != std::string::npos) col = word.size() - nl - 1;
        i = sp;
    }
    return out + "\r\n";
}

std::string EncodeFilenameParam(const std::string &fn) {
    bool ascii = true;
    for (char c : fn) {
        if (static_cast<unsigned char>(c) >= 0x80 || c == '"' || c == '\\') ascii = false;
    }
    if (ascii) return "filename=\"" + fn + "\"";
    std::string enc = "filename*=UTF-8''";
    static const char kHex[] = "0123456789ABCDEF";
    for (char c : fn) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || std::strchr("-._~", c)) {
            enc.push_back(c);
        } else {
            enc.push_back('%');
            enc.push_back(kHex[u >> 4]);
            enc.push_back(kHex[u & 15]);
        }
    }
    return enc;
}

bool NeedsQp(const std::string &body) {
    size_t line_len = 0;
    for (char c : body) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\n') {
            line_len = 0;
            continue;
        }
        if (u >= 0x80 || (u < 0x20 && c != '\t' && c != '\r')) return true;
        if (++line_len > 900) return true;
    }
    return false;
}

std::string CrlfBody(const std::string &body) {
    std::string out;
    out.reserve(body.size() + body.size() / 30);
    for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '\r') continue;
        if (body[i] == '\n') out += "\r\n";
        else out.push_back(body[i]);
    }
    if (out.size() < 2 || out.compare(out.size() - 2, 2, "\r\n") != 0) out += "\r\n";
    return out;
}

std::string TextPart(const std::string &body, std::string *headers) {
    if (NeedsQp(body)) {
        *headers = "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n";
        std::string qp = QpEncode(body);
        if (qp.size() < 2 || qp.compare(qp.size() - 2, 2, "\r\n") != 0) qp += "\r\n";
        return qp;
    }
    *headers = "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 7bit\r\n";
    return CrlfBody(body);
}

}  // namespace

std::string BuildMessage(const Draft &d, const std::vector<OutgoingFile> &files, long long now,
                         const std::string &message_id) {
    std::string m;
    m += "Date: " + FormatRfc5322Date(now) + "\r\n";
    m += FoldHeader("From", FormatAddressList(ParseAddressList(d.from)));
    const std::vector<Address> to = ParseAddressList(d.to);
    const std::vector<Address> cc = ParseAddressList(d.cc);
    if (!to.empty()) m += FoldHeader("To", FormatAddressList(to));
    if (!cc.empty()) m += FoldHeader("Cc", FormatAddressList(cc));
    if (to.empty() && cc.empty()) m += "To: undisclosed-recipients:;\r\n";
    m += FoldHeader("Subject", EncodeHeaderWords(d.subject));
    m += "Message-ID: " + message_id + "\r\n";
    if (!d.in_reply_to.empty()) m += "In-Reply-To: " + d.in_reply_to + "\r\n";
    if (!d.references.empty()) m += FoldHeader("References", d.references);
    m += "MIME-Version: 1.0\r\n";
    m += "User-Agent: mep\r\n";
    std::string text_headers;
    const std::string text = TextPart(d.body, &text_headers);
    if (files.empty()) {
        m += text_headers + "\r\n" + text;
        return m;
    }
    const std::string boundary = "=_mep_" + Base64Encode(message_id).substr(0, 24);
    std::string safe_boundary;
    for (char c : boundary) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '=') safe_boundary.push_back(c);
    }
    m += "Content-Type: multipart/mixed; boundary=\"" + safe_boundary + "\"\r\n\r\n";
    m += "This is a multi-part message in MIME format.\r\n";
    m += "--" + safe_boundary + "\r\n" + text_headers + "\r\n" + text;
    for (const OutgoingFile &f : files) {
        m += "--" + safe_boundary + "\r\n";
        m += "Content-Type: " + f.content_type + "\r\n";
        m += "Content-Transfer-Encoding: base64\r\n";
        m += "Content-Disposition: attachment; " + EncodeFilenameParam(f.filename) + "\r\n\r\n";
        m += Base64Encode(f.data, 76);
    }
    m += "--" + safe_boundary + "--\r\n";
    return m;
}

// --- IMAP ---------------------------------------------------------------

bool ImapValue::IsAtom(std::string_view a) const { return kind == Kind::Atom && IEquals(text, a); }

namespace {

class ImapReader {
public:
    explicit ImapReader(std::string_view d) : d_(d) {}
    bool AtEnd() const { return pos_ >= d_.size(); }

    // Reads one response (through its final CRLF).
    bool ReadResponse(ImapResponse *r) {
        if (AtEnd()) return false;
        const size_t line_start = pos_;
        const size_t eol = d_.find('\n', pos_);
        r->line = std::string(d_.substr(pos_, (eol == std::string_view::npos ? d_.size() : eol) - pos_));
        if (!r->line.empty() && r->line.back() == '\r') r->line.pop_back();
        SkipSpaces();
        // tag
        const size_t tstart = pos_;
        while (pos_ < d_.size() && d_[pos_] != ' ' && d_[pos_] != '\r' && d_[pos_] != '\n') pos_++;
        r->tag = std::string(d_.substr(tstart, pos_ - tstart));
        r->values.clear();
        if (r->tag != "*" && r->tag != "+") {
            // Tagged status: keep "OK"/"NO"/"BAD" and the rest of the line as atoms/text.
            SkipSpaces();
            const size_t e2 = d_.find('\n', pos_);
            std::string rest(d_.substr(pos_, (e2 == std::string_view::npos ? d_.size() : e2) - pos_));
            if (!rest.empty() && rest.back() == '\r') rest.pop_back();
            ImapValue v;
            v.kind = ImapValue::Kind::Atom;
            v.text = rest;
            r->values.push_back(v);
            pos_ = e2 == std::string_view::npos ? d_.size() : e2 + 1;
            return true;
        }
        while (!AtEnd()) {
            SkipSpaces();
            if (AtEnd()) break;
            if (d_[pos_] == '\r' || d_[pos_] == '\n') {
                if (d_[pos_] == '\r') pos_++;
                if (pos_ < d_.size() && d_[pos_] == '\n') pos_++;
                break;
            }
            ImapValue v;
            if (!ReadValue(&v, 0)) break;
            r->values.push_back(std::move(v));
        }
        if (pos_ == line_start) pos_++;  // never loop in place
        return true;
    }

private:
    void SkipSpaces() {
        while (pos_ < d_.size() && d_[pos_] == ' ') pos_++;
    }

    bool ReadValue(ImapValue *v, int depth) {
        if (depth > 50 || AtEnd()) return false;
        const char c = d_[pos_];
        if (c == '(') {
            pos_++;
            v->kind = ImapValue::Kind::List;
            while (!AtEnd()) {
                SkipSpaces();
                if (AtEnd()) break;
                if (d_[pos_] == ')') {
                    pos_++;
                    break;
                }
                if (d_[pos_] == '\r' || d_[pos_] == '\n') {
                    pos_++;  // a list never spans raw line breaks; be lenient
                    continue;
                }
                ImapValue item;
                if (!ReadValue(&item, depth + 1)) break;
                v->items.push_back(std::move(item));
            }
            return true;
        }
        if (c == '"') {
            pos_++;
            v->kind = ImapValue::Kind::String;
            while (pos_ < d_.size() && d_[pos_] != '"') {
                if (d_[pos_] == '\\' && pos_ + 1 < d_.size()) pos_++;
                if (d_[pos_] == '\r' || d_[pos_] == '\n') break;
                v->text.push_back(d_[pos_++]);
            }
            if (pos_ < d_.size() && d_[pos_] == '"') pos_++;
            return true;
        }
        if (c == '{' || (c == '~' && pos_ + 1 < d_.size() && d_[pos_ + 1] == '{')) {
            const size_t brace = d_.find('}', pos_);
            if (brace == std::string_view::npos) return false;
            const size_t num_start = pos_ + (c == '~' ? 2 : 1);
            const unsigned long long n = std::strtoull(std::string(d_.substr(num_start, brace - num_start)).c_str(), nullptr, 10);
            pos_ = brace + 1;
            if (pos_ < d_.size() && d_[pos_] == '\r') pos_++;
            if (pos_ < d_.size() && d_[pos_] == '\n') pos_++;
            const size_t take = static_cast<size_t>(std::min<unsigned long long>(n, d_.size() - pos_));
            v->kind = ImapValue::Kind::String;
            v->text = std::string(d_.substr(pos_, take));
            pos_ += take;
            return true;
        }
        // Atom; a '[' section (BODY[HEADER.FIELDS (A B)]) is read whole, including spaces.
        const size_t start = pos_;
        int bracket = 0;
        while (pos_ < d_.size()) {
            const char ch = d_[pos_];
            if (ch == '\n') break;
            if (ch == '[') bracket++;
            else if (ch == ']' && bracket > 0) bracket--;
            else if (bracket == 0 && (ch == ' ' || ch == '(' || ch == ')' || ch == '\r' || ch == '\n' || ch == '{' ||
                                      ch == '"')) {
                break;
            }
            pos_++;
        }
        if (pos_ == start) {
            pos_++;
            return false;
        }
        v->text = std::string(d_.substr(start, pos_ - start));
        v->kind = IEquals(v->text, "NIL") ? ImapValue::Kind::Nil : ImapValue::Kind::Atom;
        return true;
    }

    std::string_view d_;
    size_t pos_ = 0;
};

}  // namespace

std::vector<ImapResponse> ParseImapStream(std::string_view data) {
    std::vector<ImapResponse> out;
    ImapReader r(data);
    ImapResponse resp;
    while (r.ReadResponse(&resp)) {
        if (!resp.tag.empty()) out.push_back(resp);
        resp = ImapResponse();
    }
    return out;
}

bool MessageSummary::HasFlag(std::string_view flag) const {
    for (const std::string &f : flags) {
        if (IEquals(f, flag)) return true;
    }
    return false;
}

void MessageSummary::SetFlag(std::string_view flag, bool on) {
    if (on == HasFlag(flag)) return;
    if (on) {
        flags.emplace_back(flag);
    } else {
        flags.erase(std::remove_if(flags.begin(), flags.end(), [&](const std::string &f) { return IEquals(f, flag); }),
                    flags.end());
    }
}

std::vector<Folder> ReadListResponses(const std::vector<ImapResponse> &rs) {
    std::vector<Folder> out;
    for (const ImapResponse &r : rs) {
        if (r.tag != "*" || r.values.size() < 4 || !(r.values[0].IsAtom("LIST") || r.values[0].IsAtom("XLIST"))) continue;
        Folder f;
        for (const ImapValue &a : r.values[1].items) f.attrs.push_back(Lower(a.text));
        if (r.values[2].kind == ImapValue::Kind::String && !r.values[2].text.empty()) f.delim = r.values[2].text[0];
        f.raw = r.values[3].text;
        f.name = DecodeMailboxName(f.raw);
        for (const std::string &a : f.attrs) {
            if (a == "\\noselect" || a == "\\nonexistent") f.selectable = false;
            if (a == "\\trash") f.role = "trash";
            else if (a == "\\sent") f.role = "sent";
            else if (a == "\\archive") f.role = "archive";
            else if (a == "\\all" || a == "\\allmail") f.role = "all";
            else if (a == "\\drafts") f.role = "drafts";
            else if (a == "\\junk" || a == "\\spam") f.role = "junk";
            else if (a == "\\flagged" || a == "\\starred") f.role = "flagged";
            else if (a == "\\important") f.role = f.role.empty() ? "important" : f.role;
        }
        if (f.role.empty()) {
            std::string leaf = Lower(f.name);
            const size_t d = leaf.rfind(f.delim);
            if (d != std::string::npos) leaf = leaf.substr(d + 1);
            if (leaf == "trash" || leaf == "deleted items" || leaf == "deleted messages" || leaf == "bin") f.role = "trash";
            else if (leaf == "sent" || leaf == "sent items" || leaf == "sent mail" || leaf == "sent messages") f.role = "sent";
            else if (leaf == "archive" || leaf == "archives") f.role = "archive";
            else if (leaf == "drafts") f.role = "drafts";
            else if (leaf == "junk" || leaf == "spam" || leaf == "junk e-mail") f.role = "junk";
        }
        if (IEquals(f.raw, "INBOX")) {
            f.raw = "INBOX";
            f.name = "INBOX";
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<MessageSummary> ReadFetchSummaries(const std::vector<ImapResponse> &rs) {
    std::vector<MessageSummary> out;
    for (const ImapResponse &r : rs) {
        if (r.tag != "*" || r.values.size() < 3 || !r.values[1].IsAtom("FETCH")) continue;
        const ImapValue &list = r.values[2];
        if (list.kind != ImapValue::Kind::List) continue;
        MessageSummary s;
        s.seq = static_cast<uint32_t>(std::strtoul(r.values[0].text.c_str(), nullptr, 10));
        std::string header_block;
        for (size_t i = 0; i + 1 < list.items.size(); i += 2) {
            const ImapValue &key = list.items[i];
            const ImapValue &val = list.items[i + 1];
            const std::string k = Lower(key.text);
            if (k == "uid") s.uid = static_cast<uint32_t>(std::strtoul(val.text.c_str(), nullptr, 10));
            else if (k == "flags") {
                for (const ImapValue &f : val.items) s.flags.push_back(f.text);
            } else if (k == "internaldate") s.internal_date = ParseDate(val.text);
            else if (k == "rfc822.size") s.size = std::strtoull(val.text.c_str(), nullptr, 10);
            else if (k.rfind("body[", 0) == 0 || k.rfind("rfc822.header", 0) == 0) header_block += val.text;
        }
        if (s.uid == 0) continue;
        if (!header_block.empty()) {
            const std::vector<Header> hs = ParseHeaders(header_block);
            s.subject = DecodeHeaderWords(HeaderValue(hs, "Subject"));
            s.from = ParseAddressList(HeaderValue(hs, "From"));
            s.to = ParseAddressList(HeaderValue(hs, "To"));
            s.date = ParseDate(HeaderValue(hs, "Date"));
            s.message_id = Trim(HeaderValue(hs, "Message-ID"));
            const std::string ct = Lower(HeaderValue(hs, "Content-Type"));
            s.has_attachment = ct.rfind("multipart/mixed", 0) == 0;
        }
        if (s.date < 0) s.date = s.internal_date;
        // A later FETCH for the same UID (flags changed mid-session) updates the earlier one.
        bool merged = false;
        for (MessageSummary &e : out) {
            if (e.uid != s.uid) continue;
            e.flags = s.flags;
            if (!header_block.empty()) {
                e.subject = s.subject;
                e.from = s.from;
            }
            merged = true;
        }
        if (!merged) out.push_back(std::move(s));
    }
    return out;
}

std::vector<uint32_t> ReadSearch(const std::vector<ImapResponse> &rs) {
    std::vector<uint32_t> out;
    for (const ImapResponse &r : rs) {
        if (r.tag != "*" || r.values.empty() || !r.values[0].IsAtom("SEARCH")) continue;
        for (size_t i = 1; i < r.values.size(); i++) {
            if (r.values[i].kind != ImapValue::Kind::Atom) continue;
            const unsigned long v = std::strtoul(r.values[i].text.c_str(), nullptr, 10);
            if (v > 0) out.push_back(static_cast<uint32_t>(v));
        }
    }
    return out;
}

std::vector<std::pair<uint32_t, std::string>> ReadFetchBodies(const std::vector<ImapResponse> &rs) {
    std::vector<std::pair<uint32_t, std::string>> out;
    for (const ImapResponse &r : rs) {
        if (r.tag != "*" || r.values.size() < 3 || !r.values[1].IsAtom("FETCH")) continue;
        const ImapValue &list = r.values[2];
        if (list.kind != ImapValue::Kind::List) continue;
        uint32_t uid = 0;
        const std::string *body = nullptr;
        for (size_t i = 0; i + 1 < list.items.size(); i += 2) {
            const std::string k = Lower(list.items[i].text);
            if (k == "uid") uid = static_cast<uint32_t>(std::strtoul(list.items[i + 1].text.c_str(), nullptr, 10));
            else if (k == "body[]" || k == "body.peek[]" || k == "rfc822") body = &list.items[i + 1].text;
        }
        if (uid != 0 && body) out.emplace_back(uid, *body);
    }
    return out;
}

SelectInfo ReadSelect(const std::vector<ImapResponse> &rs) {
    SelectInfo s;
    for (const ImapResponse &r : rs) {
        if (r.tag == "*" && r.values.size() >= 2 && r.values[1].IsAtom("EXISTS")) {
            s.exists = static_cast<uint32_t>(std::strtoul(r.values[0].text.c_str(), nullptr, 10));
            s.ok = true;
        }
        const std::string up = Lower(r.line);
        const size_t v = up.find("[uidvalidity ");
        if (v != std::string::npos) s.uidvalidity = static_cast<uint32_t>(std::strtoul(up.c_str() + v + 13, nullptr, 10));
        const size_t n = up.find("[uidnext ");
        if (n != std::string::npos) s.uidnext = static_cast<uint32_t>(std::strtoul(up.c_str() + n + 9, nullptr, 10));
    }
    return s;
}

std::string ImapQuote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\r' || c == '\n') continue;
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

std::string ImapSet(std::vector<uint32_t> nums) {
    std::sort(nums.begin(), nums.end());
    nums.erase(std::unique(nums.begin(), nums.end()), nums.end());
    std::string out;
    size_t i = 0;
    while (i < nums.size()) {
        size_t j = i;
        while (j + 1 < nums.size() && nums[j + 1] == nums[j] + 1) j++;
        if (!out.empty()) out += ",";
        out += std::to_string(nums[i]);
        if (j > i) out += ":" + std::to_string(nums[j]);
        i = j + 1;
    }
    return out;
}

const char *SummaryFetchItems() {
    return "(UID FLAGS INTERNALDATE RFC822.SIZE BODY.PEEK[HEADER.FIELDS (FROM TO SUBJECT DATE MESSAGE-ID CONTENT-TYPE)])";
}

CurlTrace ParseCurlTrace(std::string_view trace) {
    CurlTrace t;
    enum class Block { None, Recv, Send };
    Block block = Block::None;
    std::string sent_bytes;
    size_t pos = 0;
    while (pos < trace.size()) {
        size_t eol = trace.find('\n', pos);
        if (eol == std::string_view::npos) eol = trace.size();
        std::string_view line = trace.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        pos = eol + 1;
        if (line.rfind("<= ", 0) == 0 || line.rfind("=> ", 0) == 0 || line.rfind("== ", 0) == 0) {
            if (line.rfind("<= Recv header", 0) == 0) block = Block::Recv;
            else if (line.rfind("=> Send header", 0) == 0) block = Block::Send;
            else block = Block::None;
            continue;
        }
        if (block == Block::None) continue;
        // "0000: 2a 20 4f ..." -- up to 16 hex pairs, then the ASCII column.
        const size_t colon = line.find(": ");
        if (colon == std::string_view::npos || colon == 0) {
            block = Block::None;
            continue;
        }
        bool hex_offset = true;
        for (size_t k = 0; k < colon; k++) hex_offset = hex_offset && HexVal(line[k]) >= 0;
        if (!hex_offset) {
            block = Block::None;
            continue;
        }
        std::string_view hex = line.substr(colon + 2);
        std::string &dst = block == Block::Recv ? t.received : sent_bytes;
        for (size_t k = 0; k < 16; k++) {
            const size_t at = k * 3;
            if (at + 2 > hex.size()) break;
            const int hi = HexVal(hex[at]);
            const int lo = HexVal(hex[at + 1]);
            if (hi < 0 || lo < 0 || (at + 2 < hex.size() && hex[at + 2] != ' ')) break;
            dst.push_back(static_cast<char>(hi * 16 + lo));
        }
    }
    // Keep tagged command lines only, minus anything carrying a credential.
    size_t p = 0;
    while (p < sent_bytes.size()) {
        size_t e = sent_bytes.find('\n', p);
        if (e == std::string::npos) e = sent_bytes.size();
        std::string line = sent_bytes.substr(p, e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p = e + 1;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) continue;
        const std::string cmd = Lower(line.substr(sp + 1));
        if (cmd.rfind("login", 0) == 0 || cmd.rfind("authenticate", 0) == 0) continue;
        bool tag_ok = std::isalpha(static_cast<unsigned char>(line[0])) != 0;
        for (size_t k = 1; k < sp; k++) tag_ok = tag_ok && std::isalnum(static_cast<unsigned char>(line[k])) != 0;
        if (tag_ok) t.sent.push_back(line);
    }
    return t;
}

std::string TaggedStatus(const CurlTrace &t, const std::vector<ImapResponse> &rs, std::string_view command_prefix) {
    std::string tag;
    for (const std::string &s : t.sent) {
        const size_t sp = s.find(' ');
        if (sp == std::string::npos) continue;
        if (IStartsWith(std::string_view(s).substr(sp + 1), command_prefix)) tag = s.substr(0, sp);
    }
    if (tag.empty()) return "";
    for (const ImapResponse &r : rs) {
        if (r.tag == tag && !r.values.empty()) return r.values[0].text;
    }
    return "";
}

// --- Accounts -----------------------------------------------------------

const std::vector<Provider> &Providers() {
    static const std::vector<Provider> kProviders = {
        {"gmail", "Gmail", "imap.gmail.com", 993, "smtp.gmail.com", 465, true,
         "Google account > Security > 2-Step Verification > App passwords (https://myaccount.google.com/apppasswords)",
         {"gmail.com", "googlemail.com"}},
        {"outlook", "Outlook / Microsoft 365", "outlook.office365.com", 993, "smtp.office365.com", 587, true,
         "Microsoft account > Security > Advanced security options > App passwords (accounts that allow IMAP)",
         {"outlook.com", "hotmail.com", "live.com", "msn.com"}},
        {"icloud", "iCloud", "imap.mail.me.com", 993, "smtp.mail.me.com", 587, false,
         "Apple Account > Sign-In and Security > App-Specific Passwords (https://account.apple.com)",
         {"icloud.com", "me.com", "mac.com"}},
        {"yahoo", "Yahoo Mail", "imap.mail.yahoo.com", 993, "smtp.mail.yahoo.com", 465, false,
         "Yahoo Account Security > Generate app password", {"yahoo.com", "ymail.com", "rocketmail.com"}},
        {"fastmail", "Fastmail", "imap.fastmail.com", 993, "smtp.fastmail.com", 465, false,
         "Fastmail Settings > Privacy & Security > App passwords", {"fastmail.com", "fastmail.fm"}},
        {"proton", "Proton Mail (Bridge)", "127.0.0.1", 1143, "127.0.0.1", 1025, false,
         "Proton Mail Bridge shows the IMAP/SMTP password to use", {"proton.me", "protonmail.com", "pm.me"}},
        {"zoho", "Zoho Mail", "imap.zoho.com", 993, "smtp.zoho.com", 465, false,
         "Zoho Accounts > Security > App Passwords", {"zoho.com", "zohomail.com"}},
        {"aol", "AOL Mail", "imap.aol.com", 993, "smtp.aol.com", 465, false, "AOL Account Security > Generate app password",
         {"aol.com"}},
        {"gmx", "GMX", "imap.gmx.com", 993, "mail.gmx.com", 465, false, "GMX settings > enable POP3/IMAP access",
         {"gmx.com", "gmx.net", "gmx.de"}},
    };
    return kProviders;
}

const Provider *ProviderForEmail(std::string_view email) {
    const size_t at = email.rfind('@');
    if (at == std::string_view::npos) return nullptr;
    const std::string domain = Lower(Trim(email.substr(at + 1)));
    for (const Provider &p : Providers()) {
        for (const std::string &d : p.domains) {
            if (d == domain) return &p;
        }
    }
    return nullptr;
}

const Provider *ProviderById(std::string_view id) {
    for (const Provider &p : Providers()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

std::string Account::FromLine() const {
    Address a;
    a.name = name;
    a.email = email;
    return FormatAddressList({a});
}

void ApplyProviderDefaults(Account *a) {
    const Provider *p = ProviderById(a->provider);
    if (!p && a->provider.empty()) {
        p = ProviderForEmail(a->email);
        if (p) a->provider = p->id;
    }
    if (!p) return;
    if (a->imap_host.empty()) {
        a->imap_host = p->imap_host;
        a->imap_port = p->imap_port;
    }
    if (a->smtp_host.empty()) {
        a->smtp_host = p->smtp_host;
        a->smtp_port = p->smtp_port;
    }
    if (p->id == "gmail") a->server_saves_sent = true;
    if (p->id == "proton") a->insecure = true;  // Bridge listens on localhost without TLS by default
}

std::vector<Account> LoadAccounts(const std::string &path, std::string *error) {
    std::vector<Account> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::ostringstream ss;
    ss << in.rdbuf();
    Json root;
    if (!Json::Parse(ss.str(), &root) || !root.is_object()) {
        if (error) *error = path + " is not valid JSON";
        return out;
    }
    const Json &list = root.get("accounts");
    for (const Json &j : list.items()) {
        if (!j.is_object()) continue;
        Account a;
        a.id = j.get("id").as_string("");
        a.name = j.get("name").as_string("");
        a.email = j.get("email").as_string("");
        a.username = j.get("username").as_string("");
        a.provider = j.get("provider").as_string("");
        a.imap_host = j.get("imap_host").as_string("");
        a.imap_port = j.get("imap_port").as_int(993);
        a.smtp_host = j.get("smtp_host").as_string("");
        a.smtp_port = j.get("smtp_port").as_int(465);
        a.secret = j.get("secret").as_string("keychain");
        a.secret_command = j.get("secret_command").as_string("");
        a.oauth2 = j.get("oauth2").as_bool(false);
        a.oauth_provider = j.get("oauth_provider").as_string("");
        a.oauth_client_id = j.get("oauth_client_id").as_string("");
        a.oauth_client_secret = j.get("oauth_client_secret").as_string("");
        a.oauth_auth_url = j.get("oauth_auth_url").as_string("");
        a.oauth_token_url = j.get("oauth_token_url").as_string("");
        a.oauth_scope = j.get("oauth_scope").as_string("");
        a.server_saves_sent = j.get("server_saves_sent").as_bool(false);
        a.insecure = j.get("insecure").as_bool(false);
        a.signature = j.get("signature").as_string("");
        a.trash = j.get("trash").as_string("");
        a.archive = j.get("archive").as_string("");
        a.sent = j.get("sent").as_string("");
        a.drafts = j.get("drafts").as_string("");
        if (a.email.empty()) continue;
        const bool had_saves_sent = j.contains("server_saves_sent");
        ApplyProviderDefaults(&a);
        if (had_saves_sent) a.server_saves_sent = j.get("server_saves_sent").as_bool(false);
        if (a.id.empty()) a.id = AccountIdFor(a.email, out);
        out.push_back(std::move(a));
    }
    return out;
}

bool SaveAccounts(const std::string &path, const std::vector<Account> &accounts) {
    // Pretty-printed by hand so the file reads (and diffs) well when edited.
    std::string s = "{\n  \"accounts\": [\n";
    for (size_t i = 0; i < accounts.size(); i++) {
        const Account &a = accounts[i];
        auto field = [&](const char *k, const Json &v, bool last = false) {
            s += std::string("      \"") + k + "\": " + v.dump() + (last ? "\n" : ",\n");
        };
        s += "    {\n";
        field("id", Json(a.id));
        field("name", Json(a.name));
        field("email", Json(a.email));
        if (!a.username.empty()) field("username", Json(a.username));
        field("provider", Json(a.provider));
        field("imap_host", Json(a.imap_host));
        field("imap_port", Json(a.imap_port));
        field("smtp_host", Json(a.smtp_host));
        field("smtp_port", Json(a.smtp_port));
        field("secret", Json(a.secret));
        if (!a.secret_command.empty()) field("secret_command", Json(a.secret_command));
        if (a.oauth2) field("oauth2", Json(true));
        if (!a.oauth_provider.empty()) field("oauth_provider", Json(a.oauth_provider));
        if (!a.oauth_client_id.empty()) field("oauth_client_id", Json(a.oauth_client_id));
        if (!a.oauth_client_secret.empty()) field("oauth_client_secret", Json(a.oauth_client_secret));
        if (!a.oauth_auth_url.empty()) field("oauth_auth_url", Json(a.oauth_auth_url));
        if (!a.oauth_token_url.empty()) field("oauth_token_url", Json(a.oauth_token_url));
        if (!a.oauth_scope.empty()) field("oauth_scope", Json(a.oauth_scope));
        if (a.insecure) field("insecure", Json(true));
        if (!a.signature.empty()) field("signature", Json(a.signature));
        if (!a.trash.empty()) field("trash", Json(a.trash));
        if (!a.archive.empty()) field("archive", Json(a.archive));
        if (!a.sent.empty()) field("sent", Json(a.sent));
        if (!a.drafts.empty()) field("drafts", Json(a.drafts));
        field("server_saves_sent", Json(a.server_saves_sent), true);
        s += i + 1 < accounts.size() ? "    },\n" : "    }\n";
    }
    s += "  ]\n}\n";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << s;
        if (!out) return false;
    }
#if !defined(_WIN32)
    chmod(tmp.c_str(), 0600);
#endif
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string AccountIdFor(const std::string &email, const std::vector<Account> &existing) {
    std::string base = Lower(email.substr(0, email.find('@')));
    std::string clean;
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') clean.push_back(c);
    }
    if (clean.empty()) clean = "mail";
    std::string id = clean;
    for (int n = 2;; n++) {
        bool taken = false;
        for (const Account &a : existing) taken = taken || a.id == id;
        if (!taken) return id;
        id = clean + std::to_string(n);
    }
}

// --- OAuth2 browser sign-in -----------------------------------------------

const std::vector<OAuthProvider> &OAuthProviders() {
    static const std::vector<OAuthProvider> kProviders = {
        {"google", "Google", "https://accounts.google.com/o/oauth2/v2/auth", "https://oauth2.googleapis.com/token",
         "https://mail.google.com/ openid email profile", "127.0.0.1", "access_type=offline&prompt=consent", true,
         "https://console.cloud.google.com/auth/clients"},
        {"microsoft", "Microsoft", "https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
         "https://login.microsoftonline.com/common/oauth2/v2.0/token",
         "https://outlook.office.com/IMAP.AccessAsUser.All https://outlook.office.com/SMTP.Send offline_access openid "
         "email profile",
         "localhost", "prompt=select_account", false,
         "https://portal.azure.com/#view/Microsoft_AAD_RegisteredApps/ApplicationsListBlade"},
    };
    return kProviders;
}

const OAuthProvider *OAuthProviderById(std::string_view id) {
    for (const OAuthProvider &p : OAuthProviders()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const OAuthProvider *OAuthProviderForMail(std::string_view mail_provider) {
    if (mail_provider == "gmail") return OAuthProviderById("google");
    if (mail_provider == "outlook") return OAuthProviderById("microsoft");
    return nullptr;
}

namespace {

std::string EnvOr(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return v && *v ? std::string(v) : std::string(fallback);
}

void BuiltinClient(std::string_view provider_id, std::string *id, std::string *secret) {
    id->clear();
    secret->clear();
    if (provider_id == "google") {
        *id = EnvOr("MEP_GOOGLE_OAUTH_CLIENT_ID", MEP_GOOGLE_OAUTH_CLIENT_ID);
        *secret = EnvOr("MEP_GOOGLE_OAUTH_CLIENT_SECRET", MEP_GOOGLE_OAUTH_CLIENT_SECRET);
    } else if (provider_id == "microsoft") {
        *id = EnvOr("MEP_MICROSOFT_OAUTH_CLIENT_ID", MEP_MICROSOFT_OAUTH_CLIENT_ID);
    }
}

}  // namespace

bool HasBuiltinOAuthClient(std::string_view provider_id) {
    std::string id, secret;
    BuiltinClient(provider_id, &id, &secret);
    const OAuthProvider *p = OAuthProviderById(provider_id);
    return !id.empty() && (!p || !p->needs_secret || !secret.empty());
}

void FillBuiltinOAuthClient(Account *a) {
    if (!a->oauth_client_id.empty() || !HasBuiltinOAuthClient(a->oauth_provider)) return;
    BuiltinClient(a->oauth_provider, &a->oauth_client_id, &a->oauth_client_secret);
}

std::string MailPresetForOAuth(std::string_view oauth_provider) {
    if (oauth_provider == "google") return "gmail";
    if (oauth_provider == "microsoft") return "outlook";
    return "";
}

void ParseIdTokenClaims(std::string_view jwt, std::string *email, std::string *name) {
    const size_t a = jwt.find('.');
    const size_t b = a == std::string_view::npos ? a : jwt.find('.', a + 1);
    if (b == std::string_view::npos) return;
    Json claims;
    if (!Json::Parse(Base64Decode(jwt.substr(a + 1, b - a - 1)), &claims) || !claims.is_object()) return;
    *email = claims.get("email").as_string("");
    // Microsoft personal accounts may carry the address only as the sign-in name.
    if (email->empty()) {
        const std::string upn = claims.get("preferred_username").as_string("");
        if (upn.find('@') != std::string::npos) *email = upn;
    }
    *name = claims.get("name").as_string("");
}

bool AccountOAuthProvider(const Account &a, OAuthProvider *out) {
    if (a.oauth_provider.empty()) return false;
    const OAuthProvider *preset = OAuthProviderById(a.oauth_provider);
    OAuthProvider p = preset ? *preset : OAuthProvider{a.oauth_provider, a.oauth_provider, "", "", "", "127.0.0.1", "", false, ""};
    if (!a.oauth_auth_url.empty()) p.auth_url = a.oauth_auth_url;
    if (!a.oauth_token_url.empty()) p.token_url = a.oauth_token_url;
    if (!a.oauth_scope.empty()) p.scope = a.oauth_scope;
    if (p.auth_url.empty() || p.token_url.empty()) return false;
    *out = p;
    return true;
}

std::string Sha256(std::string_view data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg(data);
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; i--) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (size_t i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4])) << 24) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 1])) << 16) |
                   (static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 2])) << 8) |
                   static_cast<uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + 3]));
        }
        for (size_t i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; i++) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h) {
        for (int i = 3; i >= 0; i--) out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
    }
    return out;
}

std::string Base64Url(std::string_view in) {
    std::string b = Base64Encode(in);
    while (!b.empty() && b.back() == '=') b.pop_back();
    for (char &c : b) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return b;
}

std::string PkceChallenge(std::string_view verifier) { return Base64Url(Sha256(verifier)); }

std::string UrlEncodeComponent(std::string_view s) { return UrlEncodePath(s, false); }

std::string UrlDecodeComponent(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() && HexVal(s[i + 1]) >= 0 && HexVal(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(HexVal(s[i + 1]) * 16 + HexVal(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ParseQuery(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        std::string_view part = query.substr(pos, amp - pos);
        if (!part.empty()) {
            const size_t eq = part.find('=');
            if (eq == std::string_view::npos) out.emplace_back(UrlDecodeComponent(part), "");
            else out.emplace_back(UrlDecodeComponent(part.substr(0, eq)), UrlDecodeComponent(part.substr(eq + 1)));
        }
        pos = amp + 1;
    }
    return out;
}

std::string OAuthAuthorizeUrl(const OAuthProvider &p, const std::string &client_id, const std::string &redirect_uri,
                              const std::string &state, const std::string &challenge, const std::string &login_hint) {
    std::string url = p.auth_url + (p.auth_url.find('?') == std::string::npos ? "?" : "&");
    url += "response_type=code&client_id=" + UrlEncodeComponent(client_id);
    url += "&redirect_uri=" + UrlEncodeComponent(redirect_uri);
    url += "&scope=" + UrlEncodeComponent(p.scope);
    url += "&state=" + UrlEncodeComponent(state);
    url += "&code_challenge=" + UrlEncodeComponent(challenge) + "&code_challenge_method=S256";
    if (!login_hint.empty()) url += "&login_hint=" + UrlEncodeComponent(login_hint);
    if (!p.extra_params.empty()) url += "&" + p.extra_params;
    return url;
}

OAuthTokens ParseTokenResponse(std::string_view json) {
    OAuthTokens t;
    Json j;
    if (!Json::Parse(std::string(json), &j) || !j.is_object()) {
        t.error = "the sign-in server sent no usable answer";
        return t;
    }
    t.access_token = j.get("access_token").as_string("");
    ParseIdTokenClaims(j.get("id_token").as_string(""), &t.email, &t.name);
    t.refresh_token = j.get("refresh_token").as_string("");
    t.expires_in = static_cast<long long>(j.get("expires_in").as_double(0.0));
    t.scope = j.get("scope").as_string("");
    const std::string err = j.get("error").as_string("");
    if (!err.empty()) {
        t.error = err;
        const std::string desc = j.get("error_description").as_string("");
        if (!desc.empty()) t.error += ": " + desc;
    } else if (t.access_token.empty()) {
        t.error = "the sign-in server sent no access token";
    }
    return t;
}

std::string MissingMailScope(const OAuthProvider &p, const std::string &granted) {
    if (p.id != "google" || granted.empty()) return "";
    static const std::string kGmail = "https://mail.google.com/";
    if (p.scope.find(kGmail) == std::string::npos) return "";  // an account-set scope: not ours to judge
    size_t pos = 0;
    while (pos < granted.size()) {
        size_t end = granted.find(' ', pos);
        if (end == std::string::npos) end = granted.size();
        if (granted.compare(pos, end - pos, kGmail) == 0) return "";
        pos = end + 1;
    }
    return kGmail;
}

// --- curl ---------------------------------------------------------------

std::string CurlConfigQuote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out + "\"";
}

std::string UrlEncodePath(std::string_view s, bool keep_slash) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~' || (keep_slash && c == '/')) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 15]);
        }
    }
    return out;
}

std::string ImapUrl(const Account &a, std::string_view mailbox_raw) {
    // Port 143 means STARTTLS (required unless `insecure`, see mail_net.cpp); anything else implicit TLS.
    const bool implicit_tls = !a.insecure && a.imap_port != 143;
    std::string url = std::string(implicit_tls ? "imaps://" : "imap://") + a.imap_host + ":" + std::to_string(a.imap_port) + "/";
    // curl splits the path on ';' for its own parameters and URL-decodes
    // the rest, so everything but the hierarchy '/' is percent-encoded.
    if (!mailbox_raw.empty()) url += UrlEncodePath(mailbox_raw, true);
    return url;
}

std::string SmtpUrl(const Account &a) {
    const bool implicit_tls = a.smtp_port == 465 && !a.insecure;
    return std::string(implicit_tls ? "smtps://" : "smtp://") + a.smtp_host + ":" + std::to_string(a.smtp_port);
}

// --- Safety checks --------------------------------------------------------

bool IsLoopbackHost(std::string_view host_in) {
    std::string host = Lower(Trim(host_in));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host == "localhost" || host == "::1") return true;
    // 127.0.0.0/8, written as four decimal octets.
    if (host.rfind("127.", 0) != 0) return false;
    int dots = 0;
    for (char c : host) {
        if (c == '.') dots++;
        else if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return dots == 3;
}

std::string UrlHost(std::string_view url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) return "";
    std::string_view rest = url.substr(scheme + 3);
    const size_t end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, end);
    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        return std::string(authority.substr(0, close == std::string_view::npos ? authority.size() : close + 1));
    }
    const size_t colon = authority.rfind(':');
    return std::string(colon == std::string_view::npos ? authority : authority.substr(0, colon));
}

bool IsSafeAuthUrl(std::string_view url) {
    const std::string lower = Lower(url);
    if (lower.rfind("https://", 0) == 0) return !UrlHost(url).empty();
    if (lower.rfind("http://", 0) == 0) return IsLoopbackHost(UrlHost(url));
    return false;
}

std::string CleanPastedValue(std::string_view s) {
    std::string v = Trim(s);
    while (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        v = Trim(std::string_view(v).substr(1, v.size() - 2));
    }
    return v;
}

bool LooksLikeGoogleClientId(std::string_view id) {
    static const std::string kSuffix = ".apps.googleusercontent.com";
    if (id.size() <= kSuffix.size() || id.substr(id.size() - kSuffix.size()) != kSuffix) return false;
    const std::string_view head = id.substr(0, id.size() - kSuffix.size());
    const size_t dash = head.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= head.size()) return false;
    for (size_t i = 0; i < head.size(); i++) {
        const char c = head[i];
        if (i < dash ? !std::isdigit(static_cast<unsigned char>(c)) : !(std::isalnum(static_cast<unsigned char>(c)) != 0)) {
            if (i != dash) return false;
        }
    }
    return true;
}

bool IsPlainEmail(std::string_view email) {
    const size_t at = email.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= email.size() || email.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    for (char c : email) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '"' || c == '\'' || c == '\\' || c == '`' || c == '$' || c == ';' ||
            c == '<' || c == '>' || c == '(' || c == ')' || c == ',') {
            return false;
        }
    }
    return true;
}

// --- Display helpers ----------------------------------------------------

namespace {

bool IsWide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

bool IsZeroWidth(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFE0F || cp == 0xAD;
}

}  // namespace

size_t DisplayWidth(std::string_view s) {
    size_t w = 0, i = 0;
    while (i < s.size()) {
        const uint32_t cp = NextCodePoint(s, &i);
        if (IsZeroWidth(cp)) continue;
        w += IsWide(cp) ? 2 : 1;
    }
    return w;
}

std::string FitWidth(std::string_view s_in, size_t width) {
    // Control characters (tabs, stray CR/LF in a subject) become spaces.
    std::string s;
    for (char c : s_in) s.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    std::string out;
    size_t w = 0, i = 0;
    const size_t total = DisplayWidth(s);
    const bool cut = total > width;
    const size_t budget = cut && width > 0 ? width - 1 : width;
    while (i < s.size()) {
        const size_t before = i;
        const uint32_t cp = NextCodePoint(s, &i);
        const size_t cw = IsZeroWidth(cp) ? 0 : (IsWide(cp) ? 2 : 1);
        if (w + cw > budget) break;
        out.append(s, before, i - before);
        w += cw;
    }
    if (cut && width > 0) {
        out += "\xE2\x80\xA6";  // U+2026
        w++;
    }
    while (w < width) {
        out.push_back(' ');
        w++;
    }
    return out;
}

std::string HumanSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string SafeFilename(std::string_view name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    out = Trim(out);
    while (!out.empty() && out[0] == '.') out.erase(out.begin());
    if (out.empty()) out = "attachment";
    return out;
}

}  // namespace mail
