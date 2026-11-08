#ifndef MEP_GO_PARSE_H
#define MEP_GO_PARSE_H

// mep's Go parser: a port of Go's go/scanner and go/parser (Go 1.26), so
// syntax errors carry go/parser's messages at its positions, and the tree
// is go/ast's (go_ast.h). gofmt -e is the oracle (`just go-parse-check`).
//
// One difference by design: go/parser stops at a nesting depth of 100,000
// (Go's stacks grow); this parser stops at Options::max_nest (default
// 10,000; 2,500 in sanitizer builds) with the same message, because it runs on a fixed C++ stack
// inside the editor.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "go_ast.h"

namespace goparse {

struct Error {
    int offset = 0;  // byte offset of the position
    int line = 0;    // 1-based
    int col = 0;     // 1-based, in bytes (go/token's Column)
    std::string msg;
};

// Sanitizers make every frame several times larger; keep the same 8 MB stack safe there.
#if defined(__SANITIZE_ADDRESS__)
constexpr int kDefaultMaxNest = 2500;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kDefaultMaxNest = 2500;
#else
constexpr int kDefaultMaxNest = 10000;
#endif
#else
constexpr int kDefaultMaxNest = 10000;
#endif

struct Options {
    bool all_errors = true;      // go/parser's AllErrors (gofmt -e); else at most 10, one per line
    bool parse_comments = true;  // ParseComments
    int max_nest = kDefaultMaxNest;
};

struct Result {
    std::unique_ptr<goast::Arena> arena;
    goast::File *file = nullptr;  // never null (an empty File when the package clause fails)
    std::vector<Error> errors;    // sorted by position, then message
    std::vector<int> lines;       // byte offset of each line start (go/token's line table)
    /** @brief go/token's Position: 1-based line and byte column of a Pos. */
    void LineCol(goast::Pos p, int *line, int *col) const;
    int Line(goast::Pos p) const;
};

/** @brief Parses one Go source file. */
Result ParseFile(std::string_view src, const Options &opts = {});

// --- the scanner, for tools that want tokens ---------------------------------

struct Token {
    goast::Pos pos = goast::NoPos;
    goast::Tok tok = goast::Tok::EOF_;
    std::string lit;
    goast::Pos end = goast::NoPos;  // just after the token's text in the source
};

/** @brief go/scanner over the whole file with ScanComments (semicolons inserted as Go does). */
std::vector<Token> Scan(std::string_view src, std::vector<Error> *errors = nullptr);

/** @brief strconv.QuoteRune. */
std::string QuoteRune(uint32_t r);
/** @brief strconv.Unquote for a Go string, rune or raw string literal; false when it is not one. */
bool Unquote(std::string_view lit, std::string *out);

}  // namespace goparse

#endif
