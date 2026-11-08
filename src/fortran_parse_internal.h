#ifndef MEP_FORTRAN_PARSE_INTERNAL_H
#define MEP_FORTRAN_PARSE_INTERNAL_H

// Shared between fortran_parse.cpp (source form, preprocessor, tokens) and
// fortran_parse_stmt.cpp (statements, structure). Not part of the API.

#include <string>
#include <vector>

#include "fortran_parse.h"

namespace fortran {

/** @brief Tokens of one statement's text; @p pos holds each byte's source position. */
std::vector<Token> Tokenize(const std::string &text, const std::vector<Pos> &pos, std::vector<Diag> *diags);
/** @brief Tokens of a macro expansion, all placed at the macro name's span. */
std::vector<Token> TokenizeText(const std::string &text, Pos at, Pos end);
/** @brief Classifies and parses every statement, then builds the construct tree. */
void ParseStatements(Source *src);

}  // namespace fortran

#endif  // MEP_FORTRAN_PARSE_INTERNAL_H
