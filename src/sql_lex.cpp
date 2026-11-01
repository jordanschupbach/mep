// SQLite's tokenizer (src/tokenize.c) and keyword table
// (tool/mkkeywordhash.c), ported: see sql_ast.h.
#include <algorithm>
#include <cstring>

#include "sql_ast.h"

namespace {

struct Keyword {
    const char *word;
    SqlTok type;
};

// mkkeywordhash.c's table for a default build (no ORDERED_SET_AGGREGATES:
// WITHIN is an identifier), sorted for binary search.
const Keyword kKeywords[] = {
    {"ABORT", SqlTok::Abort},
    {"ACTION", SqlTok::Action},
    {"ADD", SqlTok::Add},
    {"AFTER", SqlTok::After},
    {"ALL", SqlTok::All},
    {"ALTER", SqlTok::Alter},
    {"ALWAYS", SqlTok::Always},
    {"ANALYZE", SqlTok::Analyze},
    {"AND", SqlTok::And},
    {"AS", SqlTok::As},
    {"ASC", SqlTok::Asc},
    {"ATTACH", SqlTok::Attach},
    {"AUTOINCREMENT", SqlTok::Autoincr},
    {"BEFORE", SqlTok::Before},
    {"BEGIN", SqlTok::Begin},
    {"BETWEEN", SqlTok::Between},
    {"BY", SqlTok::By},
    {"CASCADE", SqlTok::Cascade},
    {"CASE", SqlTok::Case},
    {"CAST", SqlTok::Cast},
    {"CHECK", SqlTok::Check},
    {"COLLATE", SqlTok::Collate},
    {"COLUMN", SqlTok::ColumnKw},
    {"COMMIT", SqlTok::Commit},
    {"CONFLICT", SqlTok::Conflict},
    {"CONSTRAINT", SqlTok::Constraint},
    {"CREATE", SqlTok::Create},
    {"CROSS", SqlTok::JoinKw},
    {"CURRENT", SqlTok::Current},
    {"CURRENT_DATE", SqlTok::CtimeKw},
    {"CURRENT_TIME", SqlTok::CtimeKw},
    {"CURRENT_TIMESTAMP", SqlTok::CtimeKw},
    {"DATABASE", SqlTok::Database},
    {"DEFAULT", SqlTok::Default},
    {"DEFERRABLE", SqlTok::Deferrable},
    {"DEFERRED", SqlTok::Deferred},
    {"DELETE", SqlTok::Delete},
    {"DESC", SqlTok::Desc},
    {"DETACH", SqlTok::Detach},
    {"DISTINCT", SqlTok::Distinct},
    {"DO", SqlTok::Do},
    {"DROP", SqlTok::Drop},
    {"EACH", SqlTok::Each},
    {"ELSE", SqlTok::Else},
    {"END", SqlTok::End},
    {"ESCAPE", SqlTok::Escape},
    {"EXCEPT", SqlTok::Except},
    {"EXCLUDE", SqlTok::Exclude},
    {"EXCLUSIVE", SqlTok::Exclusive},
    {"EXISTS", SqlTok::Exists},
    {"EXPLAIN", SqlTok::Explain},
    {"FAIL", SqlTok::Fail},
    {"FILTER", SqlTok::Filter},
    {"FIRST", SqlTok::First},
    {"FOLLOWING", SqlTok::Following},
    {"FOR", SqlTok::For},
    {"FOREIGN", SqlTok::Foreign},
    {"FROM", SqlTok::From},
    {"FULL", SqlTok::JoinKw},
    {"GENERATED", SqlTok::Generated},
    {"GLOB", SqlTok::LikeKw},
    {"GROUP", SqlTok::Group},
    {"GROUPS", SqlTok::Groups},
    {"HAVING", SqlTok::Having},
    {"IF", SqlTok::If},
    {"IGNORE", SqlTok::Ignore},
    {"IMMEDIATE", SqlTok::Immediate},
    {"IN", SqlTok::In},
    {"INDEX", SqlTok::Index},
    {"INDEXED", SqlTok::Indexed},
    {"INITIALLY", SqlTok::Initially},
    {"INNER", SqlTok::JoinKw},
    {"INSERT", SqlTok::Insert},
    {"INSTEAD", SqlTok::Instead},
    {"INTERSECT", SqlTok::Intersect},
    {"INTO", SqlTok::Into},
    {"IS", SqlTok::Is},
    {"ISNULL", SqlTok::IsNull},
    {"JOIN", SqlTok::Join},
    {"KEY", SqlTok::Key},
    {"LAST", SqlTok::Last},
    {"LEFT", SqlTok::JoinKw},
    {"LIKE", SqlTok::LikeKw},
    {"LIMIT", SqlTok::Limit},
    {"MATCH", SqlTok::Match},
    {"MATERIALIZED", SqlTok::Materialized},
    {"NATURAL", SqlTok::JoinKw},
    {"NO", SqlTok::No},
    {"NOT", SqlTok::Not},
    {"NOTHING", SqlTok::Nothing},
    {"NOTNULL", SqlTok::NotNull},
    {"NULL", SqlTok::Null},
    {"NULLS", SqlTok::Nulls},
    {"OF", SqlTok::Of},
    {"OFFSET", SqlTok::Offset},
    {"ON", SqlTok::On},
    {"OR", SqlTok::Or},
    {"ORDER", SqlTok::Order},
    {"OTHERS", SqlTok::Others},
    {"OUTER", SqlTok::JoinKw},
    {"OVER", SqlTok::Over},
    {"PARTITION", SqlTok::Partition},
    {"PLAN", SqlTok::Plan},
    {"PRAGMA", SqlTok::Pragma},
    {"PRECEDING", SqlTok::Preceding},
    {"PRIMARY", SqlTok::Primary},
    {"QUERY", SqlTok::Query},
    {"RAISE", SqlTok::Raise},
    {"RANGE", SqlTok::Range},
    {"RECURSIVE", SqlTok::Recursive},
    {"REFERENCES", SqlTok::References},
    {"REGEXP", SqlTok::LikeKw},
    {"REINDEX", SqlTok::Reindex},
    {"RELEASE", SqlTok::Release},
    {"RENAME", SqlTok::Rename},
    {"REPLACE", SqlTok::Replace},
    {"RESTRICT", SqlTok::Restrict},
    {"RETURNING", SqlTok::Returning},
    {"RIGHT", SqlTok::JoinKw},
    {"ROLLBACK", SqlTok::Rollback},
    {"ROW", SqlTok::Row},
    {"ROWS", SqlTok::Rows},
    {"SAVEPOINT", SqlTok::Savepoint},
    {"SELECT", SqlTok::Select},
    {"SET", SqlTok::Set},
    {"TABLE", SqlTok::Table},
    {"TEMP", SqlTok::Temp},
    {"TEMPORARY", SqlTok::Temp},
    {"THEN", SqlTok::Then},
    {"TIES", SqlTok::Ties},
    {"TO", SqlTok::To},
    {"TRANSACTION", SqlTok::Transaction},
    {"TRIGGER", SqlTok::Trigger},
    {"UNBOUNDED", SqlTok::Unbounded},
    {"UNION", SqlTok::Union},
    {"UNIQUE", SqlTok::Unique},
    {"UPDATE", SqlTok::Update},
    {"USING", SqlTok::Using},
    {"VACUUM", SqlTok::Vacuum},
    {"VALUES", SqlTok::Values},
    {"VIEW", SqlTok::View},
    {"VIRTUAL", SqlTok::Virtual},
    {"WHEN", SqlTok::When},
    {"WHERE", SqlTok::Where},
    {"WINDOW", SqlTok::Window},
    {"WITH", SqlTok::With},
    {"WITHOUT", SqlTok::Without},
};

// tokenize.c's aiClass for ASCII.
enum CharClass : unsigned char {
    CC_X = 0, CC_KYWD0 = 1, CC_KYWD = 2, CC_DIGIT = 3, CC_DOLLAR = 4, CC_VARALPHA = 5, CC_VARNUM = 6,
    CC_SPACE = 7, CC_QUOTE = 8, CC_QUOTE2 = 9, CC_PIPE = 10, CC_MINUS = 11, CC_LT = 12, CC_GT = 13,
    CC_EQ = 14, CC_BANG = 15, CC_SLASH = 16, CC_LP = 17, CC_RP = 18, CC_SEMI = 19, CC_PLUS = 20,
    CC_STAR = 21, CC_PERCENT = 22, CC_COMMA = 23, CC_AND = 24, CC_TILDA = 25, CC_DOT = 26, CC_ID = 27,
    CC_ILLEGAL = 28, CC_NUL = 29, CC_BOM = 30,
};

const unsigned char kClass[128] = {
    29, 28, 28, 28, 28, 28, 28, 28, 28, 7,  7,  28, 7,  7,  28, 28,  //
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,  //
    7,  15, 8,  5,  4,  22, 24, 8,  17, 18, 21, 20, 23, 11, 26, 16,  //
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  5,  19, 12, 14, 13, 6,   //
    5,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,   //
    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2,  9,  28, 28, 28, 2,   //
    8,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,   //
    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2,  28, 10, 28, 25, 28,  //
};

unsigned char ClassOf(unsigned char c) {
    if (c < 128) return kClass[c];
    return c == 0xEF ? CC_BOM : CC_ID;
}

// sqlite3Isspace: 0x09-0x0d and space.
bool IsSpace(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d); }
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool IsXDigit(unsigned char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// IdChar(): alphanumerics, '_', '$' and every byte >= 0x80.
bool IdChar(unsigned char c) {
    return c >= 0x80 || IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

// Reads the byte at i, 0 past the end (the C code reads up to a NUL).
struct Cursor {
    std::string_view s;
    unsigned char at(size_t i) const { return i < s.size() ? static_cast<unsigned char>(s[i]) : 0; }
};

/** @brief sqlite3GetToken: the length of the token at the start of `z`, and its type. */
size_t GetToken(const Cursor &z, size_t p, SqlTok *type) {
    auto c0 = z.at(p);
    if (p >= z.s.size()) {
        *type = SqlTok::Eof;
        return 0;
    }
    size_t i = 0;
    switch (ClassOf(c0)) {
        case CC_SPACE:
            for (i = 1; IsSpace(z.at(p + i)); i++) {
            }
            *type = SqlTok::Space;
            return i;
        case CC_MINUS:
            if (z.at(p + 1) == '-') {
                for (i = 2; p + i < z.s.size() && z.at(p + i) != '\n'; i++) {
                }
                *type = SqlTok::Comment;
                return i;
            }
            if (z.at(p + 1) == '>') {
                *type = SqlTok::Ptr;
                return 2 + (z.at(p + 2) == '>' ? 1 : 0);
            }
            *type = SqlTok::Minus;
            return 1;
        case CC_LP: *type = SqlTok::LP; return 1;
        case CC_RP: *type = SqlTok::RP; return 1;
        case CC_SEMI: *type = SqlTok::Semi; return 1;
        case CC_PLUS: *type = SqlTok::Plus; return 1;
        case CC_STAR: *type = SqlTok::Star; return 1;
        case CC_SLASH: {
            if (z.at(p + 1) != '*' || p + 2 >= z.s.size()) {
                *type = SqlTok::Slash;
                return 1;
            }
            unsigned char c = z.at(p + 2);
            for (i = 3; (c != '*' || z.at(p + i) != '/') && p + i < z.s.size(); i++) c = z.at(p + i);
            if (p + i < z.s.size()) i++;
            *type = SqlTok::Comment;
            return i;
        }
        case CC_PERCENT: *type = SqlTok::Rem; return 1;
        case CC_EQ: *type = SqlTok::Eq; return 1 + (z.at(p + 1) == '=' ? 1 : 0);
        case CC_LT: {
            const unsigned char c = z.at(p + 1);
            if (c == '=') { *type = SqlTok::Le; return 2; }
            if (c == '>') { *type = SqlTok::Ne; return 2; }
            if (c == '<') { *type = SqlTok::LShift; return 2; }
            *type = SqlTok::Lt;
            return 1;
        }
        case CC_GT: {
            const unsigned char c = z.at(p + 1);
            if (c == '=') { *type = SqlTok::Ge; return 2; }
            if (c == '>') { *type = SqlTok::RShift; return 2; }
            *type = SqlTok::Gt;
            return 1;
        }
        case CC_BANG:
            if (z.at(p + 1) != '=') { *type = SqlTok::Illegal; return 1; }
            *type = SqlTok::Ne;
            return 2;
        case CC_PIPE:
            if (z.at(p + 1) != '|') { *type = SqlTok::BitOr; return 1; }
            *type = SqlTok::Concat;
            return 2;
        case CC_COMMA: *type = SqlTok::Comma; return 1;
        case CC_AND: *type = SqlTok::BitAnd; return 1;
        case CC_TILDA: *type = SqlTok::BitNot; return 1;
        case CC_QUOTE: {
            const unsigned char delim = c0;
            unsigned char c = 0;
            for (i = 1; p + i < z.s.size(); i++) {
                c = z.at(p + i);
                if (c == delim) {
                    if (z.at(p + i + 1) == delim) {
                        i++;
                    } else {
                        break;
                    }
                }
            }
            if (p + i >= z.s.size()) {
                *type = SqlTok::Illegal;
                return i;
            }
            *type = c == '\'' ? SqlTok::String : SqlTok::Id;
            return i + 1;
        }
        case CC_DOT:
            if (!IsDigit(z.at(p + 1))) {
                *type = SqlTok::Dot;
                return 1;
            }
            [[fallthrough]];
        case CC_DIGIT: {
            *type = SqlTok::Integer;
            if (c0 == '0' && (z.at(p + 1) == 'x' || z.at(p + 1) == 'X') && IsXDigit(z.at(p + 2))) {
                for (i = 3;; i++) {
                    if (!IsXDigit(z.at(p + i))) {
                        if (z.at(p + i) == '_') {
                            *type = SqlTok::QNumber;
                        } else {
                            break;
                        }
                    }
                }
            } else {
                for (i = 0;; i++) {
                    if (!IsDigit(z.at(p + i))) {
                        if (z.at(p + i) == '_') {
                            *type = SqlTok::QNumber;
                        } else {
                            break;
                        }
                    }
                }
                if (z.at(p + i) == '.') {
                    if (*type == SqlTok::Integer) *type = SqlTok::Float;
                    for (i++;; i++) {
                        if (!IsDigit(z.at(p + i))) {
                            if (z.at(p + i) == '_') {
                                *type = SqlTok::QNumber;
                            } else {
                                break;
                            }
                        }
                    }
                }
                if ((z.at(p + i) == 'e' || z.at(p + i) == 'E') &&
                    (IsDigit(z.at(p + i + 1)) ||
                     ((z.at(p + i + 1) == '+' || z.at(p + i + 1) == '-') && IsDigit(z.at(p + i + 2))))) {
                    if (*type == SqlTok::Integer) *type = SqlTok::Float;
                    for (i += 2;; i++) {
                        if (!IsDigit(z.at(p + i))) {
                            if (z.at(p + i) == '_') {
                                *type = SqlTok::QNumber;
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
            while (IdChar(z.at(p + i))) {
                *type = SqlTok::Illegal;
                i++;
            }
            return i;
        }
        case CC_QUOTE2: {
            unsigned char c = c0;
            for (i = 1; c != ']' && p + i < z.s.size(); i++) c = z.at(p + i);
            *type = c == ']' ? SqlTok::Id : SqlTok::Illegal;
            return i;
        }
        case CC_VARNUM:
            *type = SqlTok::Variable;
            for (i = 1; IsDigit(z.at(p + i)); i++) {
            }
            return i;
        case CC_DOLLAR:
        case CC_VARALPHA: {
            size_t n = 0;
            *type = SqlTok::Variable;
            for (i = 1; p + i < z.s.size(); i++) {
                const unsigned char c = z.at(p + i);
                if (IdChar(c)) {
                    n++;
                } else if (c == '(' && n > 0) {
                    unsigned char d = 0;
                    do {
                        i++;
                        d = z.at(p + i);
                    } while (p + i < z.s.size() && !IsSpace(d) && d != ')');
                    if (d == ')') {
                        i++;
                    } else {
                        *type = SqlTok::Illegal;
                    }
                    break;
                } else if (c == ':' && z.at(p + i + 1) == ':') {
                    i++;
                } else {
                    break;
                }
            }
            if (n == 0) *type = SqlTok::Illegal;
            return i;
        }
        case CC_KYWD0: {
            if (ClassOf(z.at(p + 1)) > CC_KYWD) {
                i = 1;
                break;
            }
            for (i = 2; ClassOf(z.at(p + i)) <= CC_KYWD && p + i < z.s.size(); i++) {
            }
            if (IdChar(z.at(p + i))) {
                i++;
                break;
            }
            *type = SqlKeywordType(z.s.substr(p, i));
            return i;
        }
        case CC_X:
            if (z.at(p + 1) == '\'') {
                *type = SqlTok::Blob;
                for (i = 2; IsXDigit(z.at(p + i)); i++) {
                }
                if (z.at(p + i) != '\'' || i % 2) {
                    *type = SqlTok::Illegal;
                    while (p + i < z.s.size() && z.at(p + i) != '\'') i++;
                }
                if (p + i < z.s.size()) i++;
                return i;
            }
            [[fallthrough]];
        case CC_KYWD:
        case CC_ID:
            i = 1;
            break;
        case CC_BOM:
            if (z.at(p + 1) == 0xbb && z.at(p + 2) == 0xbf) {
                *type = SqlTok::Space;
                return 3;
            }
            i = 1;
            break;
        case CC_NUL:
            *type = SqlTok::Illegal;
            return 1;  // tokenize.c stops at a NUL; one byte keeps us moving
        default:
            *type = SqlTok::Illegal;
            return 1;
    }
    while (IdChar(z.at(p + i))) i++;
    *type = SqlTok::Id;
    return i;
}

/** @brief tokenize.c's getToken(): the next non-space, non-comment token's type, folded to ID where it would be. */
SqlTok PeekFolded(const Cursor &z, size_t p) {
    SqlTok t = SqlTok::Space;
    while (true) {
        const size_t n = GetToken(z, p, &t);
        if (t != SqlTok::Space && t != SqlTok::Comment) break;
        p += n;
        if (n == 0) break;
    }
    if (t == SqlTok::Id || t == SqlTok::String || t == SqlTok::JoinKw || t == SqlTok::Window ||
        t == SqlTok::Over || SqlIsFallback(t)) {
        return SqlTok::Id;
    }
    return t;
}

}  // namespace

bool SqlIsFallback(SqlTok t) {
    switch (t) {
        case SqlTok::Abort: case SqlTok::Action: case SqlTok::After: case SqlTok::Analyze: case SqlTok::Asc:
        case SqlTok::Attach: case SqlTok::Before: case SqlTok::Begin: case SqlTok::By: case SqlTok::Cascade:
        case SqlTok::Cast: case SqlTok::ColumnKw: case SqlTok::Conflict: case SqlTok::Database:
        case SqlTok::Deferred: case SqlTok::Desc: case SqlTok::Detach: case SqlTok::Do: case SqlTok::Each:
        case SqlTok::End: case SqlTok::Exclusive: case SqlTok::Explain: case SqlTok::Fail: case SqlTok::For:
        case SqlTok::Ignore: case SqlTok::Immediate: case SqlTok::Initially: case SqlTok::Instead:
        case SqlTok::LikeKw: case SqlTok::Match: case SqlTok::No: case SqlTok::Plan: case SqlTok::Query:
        case SqlTok::Key: case SqlTok::Of: case SqlTok::Offset: case SqlTok::Pragma: case SqlTok::Raise:
        case SqlTok::Recursive: case SqlTok::Release: case SqlTok::Replace: case SqlTok::Restrict:
        case SqlTok::Row: case SqlTok::Rows: case SqlTok::Rollback: case SqlTok::Savepoint: case SqlTok::Temp:
        case SqlTok::Trigger: case SqlTok::Vacuum: case SqlTok::View: case SqlTok::Virtual: case SqlTok::With:
        case SqlTok::Without: case SqlTok::Nulls: case SqlTok::First: case SqlTok::Last:
        case SqlTok::Current: case SqlTok::Following: case SqlTok::Partition: case SqlTok::Preceding:
        case SqlTok::Range: case SqlTok::Unbounded: case SqlTok::Exclude: case SqlTok::Groups:
        case SqlTok::Others: case SqlTok::Ties: case SqlTok::Generated: case SqlTok::Always:
        case SqlTok::Materialized: case SqlTok::Reindex: case SqlTok::Rename: case SqlTok::CtimeKw:
        case SqlTok::If:
            return true;
        default:
            return false;
    }
}

SqlTok SqlKeywordType(std::string_view word) {
    if (word.size() < 2 || word.size() > 17) return SqlTok::Id;
    char up[18];
    for (size_t i = 0; i < word.size(); i++) {
        const char c = word[i];
        up[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    const std::string_view key(up, word.size());
    const Keyword *lo = std::begin(kKeywords);
    const Keyword *hi = std::end(kKeywords);
    const Keyword *it = std::lower_bound(lo, hi, key, [](const Keyword &k, std::string_view w) {
        return std::string_view(k.word) < w;
    });
    if (it != hi && std::string_view(it->word) == key) return it->type;
    return SqlTok::Id;
}

const std::vector<std::string> &SqlKeywordList() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out;
        for (const Keyword &k : kKeywords) out.emplace_back(k.word);
        return out;
    }();
    return list;
}

std::vector<SqlToken> SqlTokenize(std::string_view text) {
    std::vector<SqlToken> out;
    const Cursor z{text};
    size_t p = 0;
    SqlTok last = SqlTok::Eof;  // the previous token handed to the parser
    while (p < text.size()) {
        SqlTok t = SqlTok::Eof;
        size_t n = GetToken(z, p, &t);
        if (n == 0) n = 1;
        if (p + n > text.size()) n = text.size() - p;
        if (t == SqlTok::Window) {
            // analyzeWindowKeyword: WINDOW only before `name AS`.
            size_t q = p + n;
            SqlTok a = SqlTok::Space;
            while (true) {
                const size_t m = GetToken(z, q, &a);
                if (a != SqlTok::Space && a != SqlTok::Comment) {
                    q += m;
                    break;
                }
                q += m;
                if (m == 0) break;
            }
            const bool name = a == SqlTok::Id || a == SqlTok::String || a == SqlTok::JoinKw ||
                              a == SqlTok::Window || a == SqlTok::Over || SqlIsFallback(a);
            if (!name || PeekFolded(z, q) != SqlTok::As) t = SqlTok::Id;
        } else if (t == SqlTok::Over) {
            const SqlTok next = PeekFolded(z, p + n);
            if (!(last == SqlTok::RP && (next == SqlTok::LP || next == SqlTok::Id))) t = SqlTok::Id;
        } else if (t == SqlTok::Filter) {
            if (!(last == SqlTok::RP && PeekFolded(z, p + n) == SqlTok::LP)) t = SqlTok::Id;
        }
        if (t != SqlTok::Space) {
            out.push_back({t, static_cast<int>(p), static_cast<int>(n)});
            if (t != SqlTok::Comment) last = t;
        }
        p += n;
    }
    out.push_back({SqlTok::Eof, static_cast<int>(text.size()), 0});
    return out;
}

std::string SqlDequote(std::string_view raw) {
    if (raw.empty()) return std::string();
    const char q = raw[0];
    if (q == '[') {
        if (raw.size() >= 2 && raw.back() == ']') return std::string(raw.substr(1, raw.size() - 2));
        return std::string(raw.substr(1));
    }
    if (q != '"' && q != '\'' && q != '`') return std::string(raw);
    std::string out;
    for (size_t i = 1; i < raw.size(); i++) {
        if (raw[i] == q) {
            if (i + 1 < raw.size() && raw[i + 1] == q) {
                out += q;
                i++;
                continue;
            }
            break;
        }
        out += raw[i];
    }
    return out;
}

SqlLineIndex::SqlLineIndex(std::string_view text) : size_(static_cast<int>(text.size())) {
    starts_.push_back(0);
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') starts_.push_back(static_cast<int>(i + 1));
    }
}

int SqlLineIndex::Line(int offset) const {
    const auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    return static_cast<int>(it - starts_.begin()) - 1;
}

int SqlLineIndex::Col(int offset) const { return offset - starts_[static_cast<size_t>(Line(offset))]; }

int SqlLineIndex::LineStart(int line) const {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= starts_.size()) return size_;
    return starts_[static_cast<size_t>(line)];
}

int SqlLineIndex::Offset(int line, int col) const {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= starts_.size()) return size_;
    const int start = starts_[static_cast<size_t>(line)];
    const int end = static_cast<size_t>(line) + 1 < starts_.size() ? starts_[static_cast<size_t>(line) + 1] - 1 : size_;
    return std::min(start + std::max(col, 0), end);
}

std::string_view SqlParse::Text(int tok) const {
    if (tok < 0 || static_cast<size_t>(tok) >= tokens.size()) return std::string_view();
    const SqlToken &t = tokens[static_cast<size_t>(tok)];
    return std::string_view(text).substr(static_cast<size_t>(t.start), static_cast<size_t>(t.len));
}

std::string SqlParse::Upper(int tok) const {
    std::string s(Text(tok));
    for (char &c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}
