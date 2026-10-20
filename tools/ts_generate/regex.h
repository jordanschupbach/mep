// The part of the Rust regex-syntax crate (0.8.5) tree-sitter's generator
// uses: a pattern is parsed and translated to its high-level IR exactly as
// regex-syntax does, since the IR's shape -- its smart constructors merge
// adjacent literals, turn `a|b` into the class `[ab]`, a one-character
// class into a literal, lift common prefixes out of alternations -- is what
// the lexer NFA is built from. Unicode mode, `utf8(false)`; byte classes
// (`(?-u)`) are not supported.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tsgen::regex {

// hir::ClassUnicode: sorted, non-overlapping, non-adjacent inclusive ranges.
struct ClassUnicode {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;

    void Push(uint32_t lo, uint32_t hi) { ranges.push_back({lo, hi}), Canonicalize(); }
    void Canonicalize();
    void Union(const ClassUnicode &o);
    void Intersect(const ClassUnicode &o);
    void Difference(const ClassUnicode &o);
    void SymmetricDifference(const ClassUnicode &o);
    void Negate();
    void CaseFoldSimple();
    bool operator==(const ClassUnicode &) const = default;
};

struct Hir {
    enum class Kind : uint8_t { Empty, Literal, Class, Look, Repetition, Capture, Concat, Alternation } kind = Kind::Empty;
    std::string literal;  // Literal: UTF-8 bytes
    ClassUnicode cls;     // Class
    uint32_t min = 0;     // Repetition
    std::optional<uint32_t> max;
    bool greedy = true;
    uint32_t capture_index = 0;  // Capture
    std::optional<std::string> capture_name;
    std::vector<Hir> subs;  // Repetition/Capture: one; Concat/Alternation: several
    // Properties the smart constructors consult.
    std::optional<size_t> minimum_len, maximum_len;

    bool operator==(const Hir &o) const;
};

// ParserBuilder::new().case_insensitive(ci).unicode(true).utf8(false).build().parse(pattern)
bool Parse(const std::string &pattern, bool case_insensitive, Hir &out, std::string &error);

// char::is_alphabetic (the Alphabetic property), for keyword detection.
bool IsAlphabetic(uint32_t c);

}  // namespace tsgen::regex
