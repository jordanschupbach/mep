#include "formula.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {

/**
 * @brief Returns a copy of `s` with ASCII lower-case letters upper-cased (non-ASCII bytes untouched).
 * @param s the string to upper-case
 * @return the upper-cased copy
 */
std::string ToUpperAscii(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// ============================================================================
// Lexer
// ============================================================================

enum class TokKind { End, Number, String, Ident, Op };

struct Token {
    TokKind kind = TokKind::End;
    std::string text;
    double number = 0;
};

class Lexer {
public:
    /**
     * @brief Constructs a lexer that tokenizes `s`.
     * @param s the formula source text to lex; must outlive the Lexer (stored by reference)
     */
    explicit Lexer(const std::string &s) : s_(s) {}

    /**
     * @brief Skips leading whitespace and lexes the next token from the input.
     * @return the next Token, or a TokKind::End token once the input is exhausted
     */
    Token Next() {
        SkipSpaces();
        if (pos_ >= s_.size()) return Token{TokKind::End, "", 0};
        char c = s_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            return LexNumber();
        }
        if (c == '"') return LexString();
        if (c == '\'') return LexQuotedIdent();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') return LexIdent();
        return LexOp();
    }

private:
    /**
     * @brief Advances the cursor past any run of whitespace at the current position.
     */
    void SkipSpaces() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }

    /**
     * @brief Lexes a numeric literal starting at the current position.
     * @return a TokKind::Number token holding the parsed value
     */
    Token LexNumber() {
        const char *start = s_.c_str() + pos_;
        char *end = nullptr;
        double val = std::strtod(start, &end);
        size_t consumed = static_cast<size_t>(end - start);
        pos_ += consumed;
        return Token{TokKind::Number, "", val};
    }

    // Excel-style string escaping: a doubled `""` inside a quoted string
    // is one literal `"`, not two separate tokens' worth of quote.
    /**
     * @brief Lexes a double-quoted string literal starting at the current position, unescaping doubled quotes.
     * @return a TokKind::String token holding the unescaped text
     */
    Token LexString() {
        pos_++;  // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '"') {
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '"') {
                    out.push_back('"');
                    pos_ += 2;
                    continue;
                }
                pos_++;  // closing quote
                break;
            }
            out.push_back(c);
            pos_++;
        }
        Token t;
        t.kind = TokKind::String;
        t.text = out;
        return t;
    }

    // Excel-style quoted sheet name: 'My Sheet'!A1 -- needed because a
    // bare LexIdent stops at the first non-alnum/underscore/$ character,
    // so a sheet name containing a space or other punctuation (extremely
    // common in real XLSX/ODS files) could never otherwise be lexed as one
    // token. Doubled `''` is a literal apostrophe, mirroring LexString's
    // own `""` convention (this is exactly Excel's own escaping rule for
    // quoted sheet names). Returns a plain Ident token -- ParsePrimary's
    // existing IsOp("!") branch after an Ident already builds a
    // sheet-qualified ref from it, so quoting needs no parser changes.
    /**
     * @brief Lexes a single-quoted sheet name starting at the current position, unescaping doubled apostrophes.
     * @return a TokKind::Ident token holding the unescaped sheet name
     */
    Token LexQuotedIdent() {
        pos_++;  // opening quote
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '\'') {
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '\'') {
                    out.push_back('\'');
                    pos_ += 2;
                    continue;
                }
                pos_++;  // closing quote
                break;
            }
            out.push_back(c);
            pos_++;
        }
        Token t;
        t.kind = TokKind::Ident;
        t.text = out;
        return t;
    }

    // `$` is accepted mid-run so a cell reference like "$A$1" lexes as a
    // single Ident token (ParseCellAddress strips the $ signs later) --
    // a plain identifier (function/sheet name) never legitimately
    // contains one, so accepting it here unconditionally is harmless.
    /**
     * @brief Lexes an identifier (function name, sheet name, or cell reference) starting at the current position.
     * @return a TokKind::Ident token holding the identifier text
     */
    Token LexIdent() {
        size_t start = pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                pos_++;
            } else {
                break;
            }
        }
        Token t;
        t.kind = TokKind::Ident;
        t.text = s_.substr(start, pos_ - start);
        return t;
    }

    /**
     * @brief Lexes a one- or two-character operator token starting at the current position.
     * @return a TokKind::Op token holding the operator text
     */
    Token LexOp() {
        std::string two = (pos_ + 1 < s_.size()) ? s_.substr(pos_, 2) : "";
        if (two == "<>" || two == "<=" || two == ">=") {
            pos_ += 2;
            return Token{TokKind::Op, two, 0};
        }
        std::string one(1, s_[pos_]);
        pos_++;
        return Token{TokKind::Op, one, 0};
    }

    const std::string &s_;
    size_t pos_ = 0;
};

// ============================================================================
// Parser -- recursive descent, precedence climbing tier by tier:
// compare (lowest) -> concat -> add -> mul -> unary -> power -> primary
// (highest). See formula.h's own comment for the unary-vs-power ordering
// deliberate divergence from real Excel.
// ============================================================================

using NodePtr = std::shared_ptr<const FormulaNode>;

class Parser {
public:
    /**
     * @brief Constructs a parser over `text` and lexes the first token.
     * @param text the formula source text to parse
     */
    explicit Parser(const std::string &text) : lexer_(text) { Advance(); }

    /**
     * @brief Parses the full formula and checks that no trailing input remains.
     * @param error set to a description of the syntax error when parsing fails
     * @return the parsed AST root, or nullptr on error (either a parse error or unexpected trailing input)
     */
    NodePtr Parse(std::string &error) {
        NodePtr node = ParseCompare();
        if (!error_.empty()) {
            error = error_;
            return nullptr;
        }
        if (cur_.kind != TokKind::End) {
            error = "unexpected trailing input in formula";
            return nullptr;
        }
        return node;
    }

private:
    /**
     * @brief Consumes the current token and lexes the next one into `cur_`.
     */
    void Advance() { cur_ = lexer_.Next(); }
    /**
     * @brief Checks whether the current token is an operator token matching `s`.
     * @param s the operator text to match
     * @return true if the current token is an Op token equal to `s`
     */
    bool IsOp(const char *s) const { return cur_.kind == TokKind::Op && cur_.text == s; }
    /**
     * @brief Records a parse error, keeping only the first one reported.
     * @param msg the error message to record
     */
    void Fail(const std::string &msg) {
        if (error_.empty()) error_ = msg;
    }

    /**
     * @brief Builds a Number AST node.
     * @param v the numeric value
     * @return the new Number node
     */
    static NodePtr MakeNum(double v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Number;
        n->number = v;
        return n;
    }
    /**
     * @brief Builds a String AST node.
     * @param v the string literal value
     * @return the new String node
     */
    static NodePtr MakeStr(const std::string &v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::String;
        n->text = v;
        return n;
    }
    /**
     * @brief Builds a Bool AST node.
     * @param v the boolean value
     * @return the new Bool node
     */
    static NodePtr MakeBool(bool v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Bool;
        n->boolean = v;
        return n;
    }
    /**
     * @brief Builds a CellRef AST node.
     * @param ref the cell reference
     * @return the new CellRef node
     */
    static NodePtr MakeCellRef(const FormulaCellRef &ref) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::CellRef;
        n->cell = ref;
        return n;
    }
    /**
     * @brief Builds a Range AST node.
     * @param start the range's start cell reference
     * @param end the range's end cell reference
     * @return the new Range node
     */
    static NodePtr MakeRange(const FormulaCellRef &start, const FormulaCellRef &end) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Range;
        n->cell = start;
        n->range_end = end;
        return n;
    }
    /**
     * @brief Builds a BinaryOp AST node.
     * @param op the binary operator
     * @param lhs the left-hand operand
     * @param rhs the right-hand operand
     * @return the new BinaryOp node
     */
    static NodePtr MakeBinary(FormulaOp op, NodePtr lhs, NodePtr rhs) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::BinaryOp;
        n->op = op;
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }
    /**
     * @brief Builds a UnaryOp AST node.
     * @param op the unary operator
     * @param operand the operand, stored in the node's `lhs` field
     * @return the new UnaryOp node
     */
    static NodePtr MakeUnary(FormulaOp op, NodePtr operand) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::UnaryOp;
        n->op = op;
        n->lhs = std::move(operand);
        return n;
    }
    /**
     * @brief Builds a FunctionCall AST node.
     * @param name the (already upper-cased) function name
     * @param args the call's argument nodes
     * @return the new FunctionCall node
     */
    static NodePtr MakeFunc(const std::string &name, std::vector<NodePtr> args) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::FunctionCall;
        n->text = name;
        n->args = std::move(args);
        return n;
    }

    // Parses one cell-address Ident token (already consumed into `tok`)
    // into a FormulaCellRef; on failure, Fail()s and returns false.
    /**
     * @brief Parses an already-lexed identifier as a cell address into `out`.
     * @param ident the identifier text (e.g. "B12" or "$B$12")
     * @param out set to the parsed cell reference on success
     * @return true if `ident` is a valid cell address, false otherwise (Fail() is not called here)
     */
    bool AddressFromIdent(const std::string &ident, FormulaCellRef &out) {
        int r, c;
        bool ra, ca;
        if (!ParseCellAddress(ident, r, c, ra, ca)) return false;
        out.row = r;
        out.col = c;
        out.row_abs = ra;
        out.col_abs = ca;
        return true;
    }

    // Parses an optional ":addr2" range suffix onto an already-parsed
    // `start` ref (which may or may not itself be sheet-qualified --
    // range_end always inherits `start`'s sheet_name, matching how "A1:B10"
    // and "Sheet2!A1:B10" both only name the sheet once).
    /**
     * @brief Consumes an optional ":addr2" range suffix after an already-parsed cell reference.
     * @param start the already-parsed start cell reference
     * @return a CellRef node if no ':' follows, a Range node if a valid range end follows, or nullptr on error
     */
    NodePtr FinishRefOrRange(const FormulaCellRef &start) {
        if (!IsOp(":")) return MakeCellRef(start);
        Advance();
        if (cur_.kind != TokKind::Ident) {
            Fail("expected range end after ':'");
            return nullptr;
        }
        FormulaCellRef end;
        end.sheet_name = start.sheet_name;
        if (!AddressFromIdent(cur_.text, end)) {
            Fail("invalid range end '" + cur_.text + "'");
            return nullptr;
        }
        Advance();
        return MakeRange(start, end);
    }

    /**
     * @brief Parses the highest-precedence grammar tier: literals, parenthesized expressions, function calls, cell references, and ranges.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParsePrimary() {
        if (!error_.empty()) return nullptr;
        if (cur_.kind == TokKind::Number) {
            double v = cur_.number;
            Advance();
            return MakeNum(v);
        }
        if (cur_.kind == TokKind::String) {
            std::string v = cur_.text;
            Advance();
            return MakeStr(v);
        }
        if (IsOp("(")) {
            Advance();
            NodePtr e = ParseCompare();
            if (!IsOp(")")) {
                Fail("expected ')'");
                return nullptr;
            }
            Advance();
            return e;
        }
        if (cur_.kind == TokKind::Ident) {
            std::string ident = cur_.text;
            std::string upper = ToUpperAscii(ident);
            Advance();
            if (upper == "TRUE" || upper == "FALSE") {
                // Real producers write both the bare literal ("TRUE") and
                // the zero-arg function-call form ("TRUE()") -- confirmed
                // directly against real LibreOffice XLSX/ODS exports
                // (LO always emits the call form). Consume an optional
                // "()" so both parse to the same Bool node; a genuinely
                // user-defined 0-arg function happening to be named
                // TRUE/FALSE is not a real-world concern (both are
                // reserved words in every spreadsheet application this
                // engine targets).
                bool v = (upper == "TRUE");
                if (IsOp("(")) {
                    Advance();
                    if (!IsOp(")")) {
                        Fail("expected ')' after " + upper + "(");
                        return nullptr;
                    }
                    Advance();
                }
                return MakeBool(v);
            }
            if (IsOp("(")) {
                Advance();
                std::vector<NodePtr> args;
                if (!IsOp(")")) {
                    args.push_back(ParseCompare());
                    while (IsOp(",")) {
                        Advance();
                        args.push_back(ParseCompare());
                    }
                }
                if (!IsOp(")")) {
                    Fail("expected ')' after function arguments");
                    return nullptr;
                }
                Advance();
                return MakeFunc(upper, std::move(args));
            }
            if (IsOp("!")) {
                Advance();
                if (cur_.kind != TokKind::Ident) {
                    Fail("expected cell reference after '!'");
                    return nullptr;
                }
                FormulaCellRef ref;
                ref.sheet_name = ident;
                if (!AddressFromIdent(cur_.text, ref)) {
                    Fail("invalid cell reference '" + cur_.text + "'");
                    return nullptr;
                }
                Advance();
                return FinishRefOrRange(ref);
            }
            FormulaCellRef ref;
            if (AddressFromIdent(ident, ref)) return FinishRefOrRange(ref);
            Fail("unknown identifier '" + ident + "'");
            return nullptr;
        }
        Fail("unexpected token in formula");
        return nullptr;
    }

    /**
     * @brief Parses an optional right-associative '^' power expression over a primary operand.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParsePower() {
        NodePtr base = ParsePrimary();
        if (IsOp("^")) {
            Advance();
            NodePtr exp = ParseUnary();  // right-assoc: "2^-3" is valid
            return MakeBinary(FormulaOp::Pow, base, exp);
        }
        return base;
    }

    /**
     * @brief Parses an optional leading unary minus, otherwise falls through to power/primary parsing.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParseUnary() {
        if (!error_.empty()) return nullptr;
        if (IsOp("-")) {
            Advance();
            return MakeUnary(FormulaOp::Neg, ParseUnary());
        }
        return ParsePower();
    }

    /**
     * @brief Parses a left-associative chain of '*'/'/' multiplication and division.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParseMul() {
        NodePtr lhs = ParseUnary();
        while (IsOp("*") || IsOp("/")) {
            FormulaOp op = IsOp("*") ? FormulaOp::Mul : FormulaOp::Div;
            Advance();
            lhs = MakeBinary(op, lhs, ParseUnary());
        }
        return lhs;
    }

    /**
     * @brief Parses a left-associative chain of '+'/'-' addition and subtraction.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParseAdd() {
        NodePtr lhs = ParseMul();
        while (IsOp("+") || IsOp("-")) {
            FormulaOp op = IsOp("+") ? FormulaOp::Add : FormulaOp::Sub;
            Advance();
            lhs = MakeBinary(op, lhs, ParseMul());
        }
        return lhs;
    }

    /**
     * @brief Parses a left-associative chain of '&' string concatenation.
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParseConcat() {
        NodePtr lhs = ParseAdd();
        while (IsOp("&")) {
            Advance();
            lhs = MakeBinary(FormulaOp::Concat, lhs, ParseAdd());
        }
        return lhs;
    }

    /**
     * @brief Parses the lowest-precedence grammar tier: a chain of comparison operators (=, <>, <, >, <=, >=).
     * @return the parsed AST node, or nullptr on error
     */
    NodePtr ParseCompare() {
        NodePtr lhs = ParseConcat();
        for (;;) {
            FormulaOp op;
            if (IsOp("=")) op = FormulaOp::Eq;
            else if (IsOp("<>")) op = FormulaOp::Ne;
            else if (IsOp("<=")) op = FormulaOp::Le;
            else if (IsOp(">=")) op = FormulaOp::Ge;
            else if (IsOp("<")) op = FormulaOp::Lt;
            else if (IsOp(">")) op = FormulaOp::Gt;
            else break;
            Advance();
            lhs = MakeBinary(op, lhs, ParseConcat());
        }
        return lhs;
    }

    Lexer lexer_;
    Token cur_;
    std::string error_;
};

}  // namespace

std::shared_ptr<const FormulaNode> ParseFormula(const std::string &text, std::string &error) {
    Parser parser(text);
    return parser.Parse(error);
}

int ColumnLettersToIndex(const std::string &letters) {
    int result = 0;
    for (char c : letters) {
        if (c < 'A' || c > 'Z') return -1;
        result = result * 26 + (c - 'A' + 1);
    }
    return result - 1;
}

std::string ColumnIndexToLetters(int col) {
    std::string s;
    int n = col + 1;
    while (n > 0) {
        int rem = (n - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + rem));
        n = (n - 1) / 26;
    }
    return s;
}

bool ParseCellAddress(const std::string &text, int &row, int &col, bool &row_abs, bool &col_abs) {
    size_t i = 0, n = text.size();
    row_abs = col_abs = false;
    if (i < n && text[i] == '$') {
        col_abs = true;
        i++;
    }
    size_t letters_start = i;
    while (i < n && std::isalpha(static_cast<unsigned char>(text[i]))) i++;
    if (i == letters_start) return false;
    std::string letters = text.substr(letters_start, i - letters_start);
    for (char &c : letters) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (i < n && text[i] == '$') {
        row_abs = true;
        i++;
    }
    size_t digits_start = i;
    while (i < n && std::isdigit(static_cast<unsigned char>(text[i]))) i++;
    if (i == digits_start || i != n) return false;
    int row1 = std::atoi(text.substr(digits_start, i - digits_start).c_str());
    if (row1 < 1) return false;
    int c = ColumnLettersToIndex(letters);
    if (c < 0) return false;
    col = c;
    row = row1 - 1;
    return true;
}

std::string CellAddressToString(int row, int col) { return ColumnIndexToLetters(col) + std::to_string(row + 1); }

// ============================================================================
// Serialization -- AST back to text. Two consumers: sheet_xlsx.cpp's
// shared-formula expansion (native style, round-tripping through this
// engine's own parser) and sheet_ods.cpp's save-back (ods_style, ODF
// bracket-ref syntax). See formula.h's own comments for why parens are
// always added rather than computed minimally.
// ============================================================================

namespace {

/**
 * @brief Formats a double as formula-safe text using up to 10 significant digits.
 * @param n the number to format
 * @return the formatted text
 */
std::string FormatNumberForFormula(double n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", n);
    return std::string(buf);
}

/**
 * @brief Quotes `s` as a formula double-quoted string literal, doubling any embedded '"'.
 * @param s the raw string value
 * @return the quoted, escaped literal text including surrounding double quotes
 */
std::string QuoteStringLiteral(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

/**
 * @brief Checks whether `name` must be single-quoted when serialized (empty or containing non alnum/'_'/'$' characters).
 * @param name the sheet name to check
 * @return true if `name` needs quoting
 */
bool SheetNameNeedsQuote(const std::string &name) {
    if (name.empty()) return true;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$')) return true;
    }
    return false;
}

/**
 * @brief Wraps `name` in single quotes, doubling any embedded apostrophe.
 * @param name the sheet name to quote
 * @return the quoted, escaped sheet name including surrounding single quotes
 */
std::string QuoteSheetName(const std::string &name) {
    std::string out = "'";
    for (char c : name) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

/**
 * @brief Formats a sheet name for serialization, quoting it only if needed.
 * @param name the sheet name to format
 * @return `name` single-quoted if SheetNameNeedsQuote(name) is true, otherwise `name` unchanged
 */
std::string FormatSheetName(const std::string &name) { return SheetNameNeedsQuote(name) ? QuoteSheetName(name) : name; }

/**
 * @brief Formats a cell reference's address text ("A1", "$A$1", ...), without any sheet qualifier.
 * @param ref the cell reference to format
 * @return the "A1"-style address text, with '$' prefixes for absolute row/column
 */
std::string FormatCellRefText(const FormulaCellRef &ref) {
    std::string s;
    if (ref.col_abs) s += "$";
    s += ColumnIndexToLetters(ref.col);
    if (ref.row_abs) s += "$";
    s += std::to_string(ref.row + 1);
    return s;
}

/**
 * @brief Maps a FormulaOp to its formula-text operator symbol.
 * @param op the operator to map
 * @return the operator's text symbol (e.g. "+", "<>"), or "" for an unrecognized value
 */
std::string OpToText(FormulaOp op) {
    switch (op) {
        case FormulaOp::Add: return "+";
        case FormulaOp::Sub: return "-";
        case FormulaOp::Mul: return "*";
        case FormulaOp::Div: return "/";
        case FormulaOp::Pow: return "^";
        case FormulaOp::Concat: return "&";
        case FormulaOp::Eq: return "=";
        case FormulaOp::Ne: return "<>";
        case FormulaOp::Lt: return "<";
        case FormulaOp::Gt: return ">";
        case FormulaOp::Le: return "<=";
        case FormulaOp::Ge: return ">=";
        case FormulaOp::Neg: return "-";
    }
    return "";
}

/**
 * @brief Serializes a cell reference in this engine's native syntax ("A1" or "Sheet2!A1").
 * @param ref the cell reference to serialize
 * @return the serialized reference text
 */
std::string SerializeCellRefNative(const FormulaCellRef &ref) {
    std::string s;
    if (!ref.sheet_name.empty()) s += FormatSheetName(ref.sheet_name) + "!";
    s += FormatCellRefText(ref);
    return s;
}

// ODS bracket form: "[.A1]" (same sheet) or "[Sheet2.A1]" (qualified). A
// range's end side is always left unqualified ("[Sheet2.A1:.B2]") --
// range_end.sheet_name always mirrors start's (see FinishRefOrRange), so
// there's never a genuinely different end-sheet to print here.
/**
 * @brief Serializes a cell reference in ODF bracket syntax ("[.A1]" or "[Sheet2.A1]").
 * @param ref the cell reference to serialize
 * @return the serialized bracket-form reference text
 */
std::string SerializeCellRefOds(const FormulaCellRef &ref) {
    std::string s = "[";
    if (!ref.sheet_name.empty()) s += FormatSheetName(ref.sheet_name);
    s += "." + FormatCellRefText(ref) + "]";
    return s;
}

/**
 * @brief Serializes a range in ODF bracket syntax ("[.A1:.B2]" or "[Sheet2.A1:.B2]").
 * @param start the range's start cell reference (its sheet_name qualifies the whole range)
 * @param end the range's end cell reference (printed unqualified)
 * @return the serialized bracket-form range text
 */
std::string SerializeRangeOds(const FormulaCellRef &start, const FormulaCellRef &end) {
    std::string s = "[";
    if (!start.sheet_name.empty()) s += FormatSheetName(start.sheet_name);
    s += "." + FormatCellRefText(start) + ":." + FormatCellRefText(end) + "]";
    return s;
}

/**
 * @brief Recursively serializes an AST node to formula text, in either native or ODS-bracket style.
 * @param node the AST node to serialize
 * @param ods_style true to emit ODF bracket-ref syntax and ';' argument separators, false for native syntax and ',' separators
 * @return the serialized formula text for `node` and its subtree
 */
std::string SerializeFormulaImpl(const FormulaNode &node, bool ods_style) {
    switch (node.kind) {
        case FormulaNodeKind::Number:
            return FormatNumberForFormula(node.number);
        case FormulaNodeKind::String:
            return QuoteStringLiteral(node.text);
        case FormulaNodeKind::Bool:
            return node.boolean ? "TRUE" : "FALSE";
        case FormulaNodeKind::CellRef:
            return ods_style ? SerializeCellRefOds(node.cell) : SerializeCellRefNative(node.cell);
        case FormulaNodeKind::Range:
            return ods_style ? SerializeRangeOds(node.cell, node.range_end)
                              : (SerializeCellRefNative(node.cell) + ":" + FormatCellRefText(node.range_end));
        case FormulaNodeKind::UnaryOp:
            return "-(" + SerializeFormulaImpl(*node.lhs, ods_style) + ")";
        case FormulaNodeKind::BinaryOp:
            return "(" + SerializeFormulaImpl(*node.lhs, ods_style) + OpToText(node.op) +
                   SerializeFormulaImpl(*node.rhs, ods_style) + ")";
        case FormulaNodeKind::FunctionCall: {
            std::string s = node.text + "(";
            const char *sep = ods_style ? ";" : ",";
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i) s += sep;
                s += SerializeFormulaImpl(*node.args[i], ods_style);
            }
            s += ")";
            return s;
        }
    }
    return "";
}

}  // namespace

std::string SerializeFormula(const std::shared_ptr<const FormulaNode> &node, bool ods_style) {
    if (!node) return "";
    return SerializeFormulaImpl(*node, ods_style);
}

std::shared_ptr<const FormulaNode> ShiftFormulaRefs(const std::shared_ptr<const FormulaNode> &node, int dr, int dc) {
    if (!node) return node;
    auto n = std::make_shared<FormulaNode>(*node);  // scalar-field copy; children reassigned below
    switch (n->kind) {
        case FormulaNodeKind::CellRef:
            if (!n->cell.row_abs) n->cell.row += dr;
            if (!n->cell.col_abs) n->cell.col += dc;
            break;
        case FormulaNodeKind::Range:
            if (!n->cell.row_abs) n->cell.row += dr;
            if (!n->cell.col_abs) n->cell.col += dc;
            if (!n->range_end.row_abs) n->range_end.row += dr;
            if (!n->range_end.col_abs) n->range_end.col += dc;
            break;
        case FormulaNodeKind::UnaryOp:
            n->lhs = ShiftFormulaRefs(n->lhs, dr, dc);
            break;
        case FormulaNodeKind::BinaryOp:
            n->lhs = ShiftFormulaRefs(n->lhs, dr, dc);
            n->rhs = ShiftFormulaRefs(n->rhs, dr, dc);
            break;
        case FormulaNodeKind::FunctionCall: {
            std::vector<std::shared_ptr<const FormulaNode>> new_args;
            new_args.reserve(n->args.size());
            for (const auto &a : n->args) new_args.push_back(ShiftFormulaRefs(a, dr, dc));
            n->args = std::move(new_args);
            break;
        }
        default:
            break;
    }
    return n;
}
