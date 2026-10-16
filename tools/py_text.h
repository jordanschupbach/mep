#ifndef MEP_TOOLS_PY_TEXT_H
#define MEP_TOOLS_PY_TEXT_H

// Shared text helpers for the build/dev host tools in tools/*.cpp
// (check_help, gen_reference_pages, amalgamate). Those tools replaced
// Python scripts and must produce byte-identical output, so these helpers
// reproduce the exact Python semantics the scripts relied on:
//
//   - pathlib.read_text()/open() in text mode: UTF-8 decoding (invalid
//     bytes become U+FFFD, like errors="replace") and universal newlines
//     ("\r\n" and a lone "\r" both read as "\n");
//   - str.isspace() / the `\s` regex class, str.split() with no argument,
//     str.strip() and str.splitlines(), all of which are Unicode-aware;
//   - the `\w` class `\b` is built on.
//
// Everything works on std::u32string (one element per code point) so that
// slicing and length limits count characters the way Python does.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace pytext {

/** @brief Reads a whole file as raw bytes.
 *  @param path the file to read.
 *  @param out receives the bytes.
 *  @return false if the file could not be opened. */
inline bool ReadBytes(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

/** @brief Writes bytes to a file, replacing it.
 *  @return false on failure. */
inline bool WriteBytes(const std::string &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

/** @brief Decodes UTF-8, replacing each invalid sequence with U+FFFD. */
inline std::u32string Decode(const std::string &bytes) {
    std::u32string out;
    out.reserve(bytes.size());
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char32_t>(b0));
            ++i;
            continue;
        }
        size_t len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            len = 2;
            cp = b0 & 0x1Fu;
            min = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            len = 3;
            cp = b0 & 0x0Fu;
            min = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            len = 4;
            cp = b0 & 0x07u;
            min = 0x10000;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        size_t k = 1;
        for (; k < len && i + k < n; ++k) {
            const auto b = static_cast<unsigned char>(bytes[i + k]);
            if ((b & 0xC0u) != 0x80u) break;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (k < len || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
            i += (k > 1 ? k : 1);
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

/** @brief Encodes code points as UTF-8. */
inline std::string Encode(const std::u32string &s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0u | (c >> 12)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (c >> 18)));
            out.push_back(static_cast<char>(0x80u | ((c >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return out;
}

/** @brief An ASCII string literal as code points. */
inline std::u32string U(const std::string &ascii) {
    return std::u32string(ascii.begin(), ascii.end());
}

/** @brief Python's universal-newline translation: CRLF and CR -> LF. */
inline std::u32string UniversalNewlines(const std::u32string &s) {
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == U'\r') {
            out.push_back(U'\n');
            if (i + 1 < s.size() && s[i + 1] == U'\n') ++i;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

/** @brief Path.read_text() / open(..., "r"): decoded, universal newlines.
 *  @return false if the file could not be read. */
inline bool ReadText(const std::string &path, std::u32string *out) {
    std::string bytes;
    if (!ReadBytes(path, &bytes)) return false;
    *out = UniversalNewlines(Decode(bytes));
    return true;
}

/** @brief str.isspace() for one character (also what `\s` matches). */
inline bool IsSpace(char32_t c) {
    if (c == U' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F)) return true;
    if (c < 0x80) return false;
    return c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/** @brief Python's `\w` for str patterns: alphanumerics and underscore.
 *  Exact for ASCII. Non-ASCII code points count as word characters except
 *  for the common punctuation/symbol/space blocks -- an approximation of
 *  str.isalnum() that only matters for an identifier glued to non-ASCII
 *  text, which mep's sources do not have. */
inline bool IsWord(char32_t c) {
    if (c < 0x80) {
        return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') ||
               c == U'_';
    }
    if (IsSpace(c)) return false;
    if (c <= 0xBF) return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA ||
                          c == 0xBC || c == 0xBD || c == 0xBE;
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;
    if (c >= 0xE000 && c <= 0xF8FF) return false;
    if (c >= 0xFE00 && c <= 0xFE0F) return false;
    if (c >= 0xFF00 && c <= 0xFF0F) return false;
    if (c >= 0xFFF0 && c <= 0xFFFF) return false;
    if (c >= 0x1F000 && c <= 0x1FFFF) return false;
    return true;
}

/** @brief str.strip() with no argument. */
inline std::u32string Strip(const std::u32string &s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

/** @brief str.rstrip() with no argument. */
inline std::u32string RStrip(const std::u32string &s) {
    size_t e = s.size();
    while (e > 0 && IsSpace(s[e - 1])) --e;
    return s.substr(0, e);
}

/** @brief str.lstrip(chars). */
inline std::u32string LStripChars(const std::u32string &s, const std::u32string &chars) {
    size_t b = 0;
    while (b < s.size() && chars.find(s[b]) != std::u32string::npos) ++b;
    return s.substr(b);
}

/** @brief str.rstrip(chars). */
inline std::u32string RStripChars(const std::u32string &s, const std::u32string &chars) {
    size_t e = s.size();
    while (e > 0 && chars.find(s[e - 1]) != std::u32string::npos) --e;
    return s.substr(0, e);
}

/** @brief str.strip(chars). */
inline std::u32string StripChars(const std::u32string &s, const std::u32string &chars) {
    return LStripChars(RStripChars(s, chars), chars);
}

/** @brief str.split() with no argument: runs of whitespace separate,
 *  leading/trailing whitespace yields no empty fields. */
inline std::vector<std::u32string> SplitWs(const std::u32string &s) {
    std::vector<std::u32string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && IsSpace(s[i])) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && !IsSpace(s[j])) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

/** @brief sep.join(parts). */
inline std::u32string Join(const std::vector<std::u32string> &parts, const std::u32string &sep) {
    std::u32string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

/** @brief " ".join(s.split()) -- collapse every whitespace run. */
inline std::u32string CollapseWs(const std::u32string &s) {
    return Join(SplitWs(s), U" ");
}

/** @brief str.split(ch) on a single separator character. */
inline std::vector<std::u32string> SplitChar(const std::u32string &s, char32_t ch) {
    std::vector<std::u32string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ch) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

/** @brief Is `c` a line boundary for str.splitlines()? ("\r\n" is
 *  handled by the caller.) */
inline bool IsLineBreak(char32_t c) {
    return c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D ||
           c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
}

/** @brief str.splitlines(keepends=True). */
inline std::vector<std::u32string> SplitLinesKeepEnds(const std::u32string &s) {
    std::vector<std::u32string> out;
    size_t start = 0, i = 0;
    while (i < s.size()) {
        if (IsLineBreak(s[i])) {
            size_t end = i + 1;
            if (s[i] == U'\r' && end < s.size() && s[end] == U'\n') ++end;
            out.push_back(s.substr(start, end - start));
            start = i = end;
        } else {
            ++i;
        }
    }
    if (start < s.size()) out.push_back(s.substr(start));
    return out;
}

/** @brief Does `s` contain `needle` at position `pos`? */
inline bool At(const std::u32string &s, size_t pos, const std::u32string &needle) {
    return pos <= s.size() && s.compare(pos, needle.size(), needle) == 0;
}

/** @brief ASCII-only lower-casing (enough for the case-insensitive tag
 *  matches the tools do). */
inline char32_t AsciiLower(char32_t c) {
    return (c >= U'A' && c <= U'Z') ? c + 32 : c;
}

/** @brief Case-insensitive (ASCII) match of `needle` at `pos`. */
inline bool AtNoCase(const std::u32string &s, size_t pos, const std::u32string &needle) {
    if (pos > s.size() || s.size() - pos < needle.size()) return false;
    for (size_t i = 0; i < needle.size(); ++i) {
        if (AsciiLower(s[pos + i]) != AsciiLower(needle[i])) return false;
    }
    return true;
}

/** @brief Case-insensitive find of `needle` from `pos`. */
inline size_t FindNoCase(const std::u32string &s, const std::u32string &needle, size_t pos) {
    if (needle.size() > s.size()) return std::u32string::npos;
    for (size_t i = pos; i + needle.size() <= s.size(); ++i) {
        if (AtNoCase(s, i, needle)) return i;
    }
    return std::u32string::npos;
}

}  // namespace pytext

#endif  // MEP_TOOLS_PY_TEXT_H
