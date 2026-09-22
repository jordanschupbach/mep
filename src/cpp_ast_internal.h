#ifndef MEP_CPP_AST_INTERNAL_H
#define MEP_CPP_AST_INTERNAL_H

#include <string>
#include <vector>

#include "cpp_ast.h"

// The seam between the two halves of mep's C++ front end: cpp_ast.cpp
// owns translation phases 1-4 (splices, tokens, directives, conditional
// groups, macro expansion) and cpp_parse.cpp owns the grammar above them.
// Nothing outside those two files includes this header -- cpp_ast.h is
// the public surface.

// What the preprocessor hands the parser.
struct CppPreprocessOutput {
    // The parse stream: active tokens, macros expanded, directives
    // removed, always ending with a single End token.
    std::vector<CppToken> tokens;
    std::vector<CppDirective> directives;
    std::vector<CppInclude> includes;
    // The macro table as it stands at end of file (a `#undef` removes an
    // entry), which is what hover and completion want.
    std::map<std::string, CppMacro> macros;
    // Per physical line: whether it survived conditional compilation.
    std::vector<bool> line_active;
    // An `#if` that never closed, or an expansion that hit the token
    // budget -- either way the parser is looking at a partial file.
    bool truncated = false;
};

/**
 * @brief Runs translation phases 3-4 over a token stream: directives, conditional groups and macro expansion.
 * @param raw the tokenizer's output, directive tokens included
 * @param lines the source, used only for its line count and for include resolution
 * @param opts include-resolution and predefined-macro options
 * @param out_errors receives preprocessor errors (stray `#endif`, unterminated `#if`, `#error`, redefinitions)
 * @param out the parse stream and everything the analysis half reads about directives
 */
void CppPreprocess(const std::vector<CppToken> &raw, const std::vector<std::string> &lines,
                   const CppParseOptions &opts, std::vector<CppSyntaxError> *out_errors,
                   CppPreprocessOutput *out);

#endif  // MEP_CPP_AST_INTERNAL_H
