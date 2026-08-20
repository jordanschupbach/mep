#include "formula.h"

#include <cctype>
#include <cstdlib>

namespace {

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
    explicit Lexer(const std::string &s) : s_(s) {}

    Token Next() {
        SkipSpaces();
        if (pos_ >= s_.size()) return Token{TokKind::End, "", 0};
        char c = s_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            return LexNumber();
        }
        if (c == '"') return LexString();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') return LexIdent();
        return LexOp();
    }

private:
    void SkipSpaces() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }

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

    // `$` is accepted mid-run so a cell reference like "$A$1" lexes as a
    // single Ident token (ParseCellAddress strips the $ signs later) --
    // a plain identifier (function/sheet name) never legitimately
    // contains one, so accepting it here unconditionally is harmless.
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
    explicit Parser(const std::string &text) : lexer_(text) { Advance(); }

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
    void Advance() { cur_ = lexer_.Next(); }
    bool IsOp(const char *s) const { return cur_.kind == TokKind::Op && cur_.text == s; }
    void Fail(const std::string &msg) {
        if (error_.empty()) error_ = msg;
    }

    static NodePtr MakeNum(double v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Number;
        n->number = v;
        return n;
    }
    static NodePtr MakeStr(const std::string &v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::String;
        n->text = v;
        return n;
    }
    static NodePtr MakeBool(bool v) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Bool;
        n->boolean = v;
        return n;
    }
    static NodePtr MakeCellRef(const FormulaCellRef &ref) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::CellRef;
        n->cell = ref;
        return n;
    }
    static NodePtr MakeRange(const FormulaCellRef &start, const FormulaCellRef &end) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::Range;
        n->cell = start;
        n->range_end = end;
        return n;
    }
    static NodePtr MakeBinary(FormulaOp op, NodePtr lhs, NodePtr rhs) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::BinaryOp;
        n->op = op;
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }
    static NodePtr MakeUnary(FormulaOp op, NodePtr operand) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::UnaryOp;
        n->op = op;
        n->lhs = std::move(operand);
        return n;
    }
    static NodePtr MakeFunc(const std::string &name, std::vector<NodePtr> args) {
        auto n = std::make_shared<FormulaNode>();
        n->kind = FormulaNodeKind::FunctionCall;
        n->text = name;
        n->args = std::move(args);
        return n;
    }

    // Parses one cell-address Ident token (already consumed into `tok`)
    // into a FormulaCellRef; on failure, Fail()s and returns false.
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
            if (upper == "TRUE") return MakeBool(true);
            if (upper == "FALSE") return MakeBool(false);
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

    NodePtr ParsePower() {
        NodePtr base = ParsePrimary();
        if (IsOp("^")) {
            Advance();
            NodePtr exp = ParseUnary();  // right-assoc: "2^-3" is valid
            return MakeBinary(FormulaOp::Pow, base, exp);
        }
        return base;
    }

    NodePtr ParseUnary() {
        if (!error_.empty()) return nullptr;
        if (IsOp("-")) {
            Advance();
            return MakeUnary(FormulaOp::Neg, ParseUnary());
        }
        return ParsePower();
    }

    NodePtr ParseMul() {
        NodePtr lhs = ParseUnary();
        while (IsOp("*") || IsOp("/")) {
            FormulaOp op = IsOp("*") ? FormulaOp::Mul : FormulaOp::Div;
            Advance();
            lhs = MakeBinary(op, lhs, ParseUnary());
        }
        return lhs;
    }

    NodePtr ParseAdd() {
        NodePtr lhs = ParseMul();
        while (IsOp("+") || IsOp("-")) {
            FormulaOp op = IsOp("+") ? FormulaOp::Add : FormulaOp::Sub;
            Advance();
            lhs = MakeBinary(op, lhs, ParseMul());
        }
        return lhs;
    }

    NodePtr ParseConcat() {
        NodePtr lhs = ParseAdd();
        while (IsOp("&")) {
            Advance();
            lhs = MakeBinary(FormulaOp::Concat, lhs, ParseAdd());
        }
        return lhs;
    }

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
