#ifndef MEP_KOTLIN_PARSE_H
#define MEP_KOTLIN_PARSE_H

// mep's Kotlin parser: a port of the Kotlin compiler's own lexer (Kotlin.flex)
// and PSI parser (KotlinParsing, KotlinExpressionParsing) over an emulation of
// IntelliJ's PsiBuilder (markers, rollback, token remapping, and the
// whitespace/comment edge binders). It builds the same PSI tree as kotlinc
// (kotlin_psi.h), with the same ERROR_ELEMENTs: the same messages at the same
// positions. `just kotlin-parse-check` compares both against the compiler
// (tools/ts_grammar_diff/KotlinCheck.java) over the corpus and mutants.
//
// Line ends: the PSI works on \n-normalised text. Parse() normalises \r\n and
// lone \r first; tree offsets are into KtTree::text (the normalised text) and
// KtTree::ToOriginal maps one back. Line/column of errors are the same in both.
//
// One difference by design: the JVM parser overflows its stack on deep
// nesting (kotlinc then reports an exception); this parser stops at a depth
// of Options::max_depth (default 3,000; 1,000 in sanitizer builds),
// reports "Nesting too deep for mep's parser (more than N levels)" at the
// token where it stopped, covers the rest of the input with that error
// element, and sets KtTree::depth_limited.
//
// Ported from the Kotlin compiler (JetBrains, Apache License 2.0),
// KotlinParser.java, KotlinParsing.java, KotlinExpressionParsing.java,
// Kotlin.flex, version 2.4.10.

#include <memory>
#include <string_view>

#include "kotlin_psi.h"

namespace kt {

#if defined(__SANITIZE_ADDRESS__)
constexpr int kDefaultMaxDepth = 1000;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kDefaultMaxDepth = 1000;
#else
constexpr int kDefaultMaxDepth = 3000;
#endif
#else
constexpr int kDefaultMaxDepth = 3000;
#endif

enum class KtFileKind {
    File,    // a .kt file (KotlinParsing.parseFile)
    Script,  // a .kts file (parseScript: top-level statements inside SCRIPT > BLOCK)
};

struct KtParseOptions {
    KtFileKind kind = KtFileKind::File;
    int max_depth = kDefaultMaxDepth;
};

/** @brief Parses Kotlin source. Never returns null; the tree always covers the whole text. */
std::unique_ptr<KtTree> KtParse(std::string_view source, const KtParseOptions &opts = {});

/** @brief KtFileKind::Script for a path ending in .kts, else File (as kotlinc decides). */
KtFileKind KtKindForPath(std::string_view path);

// --- the lexer, for tools that want tokens -------------------------------------

struct KtLexeme {
    KtType type;
    uint32_t start;  // byte offset; the token ends where the next one starts
};

/** @brief Kotlin.flex over \n-normalised text: every token, whitespace and comment included
 *  (zero-width DANGLING_NEWLINE tokens too). The last element is a sentinel {NONE, size}. */
std::vector<KtLexeme> KtLex(std::string_view text);

}  // namespace kt

#endif
