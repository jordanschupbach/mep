// COBOL line structure: see cobol_syntax.h.
#include "cobol_syntax.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace {

const char *const kKeywords[] = {
    // Divisions, sections and headers.
    "IDENTIFICATION", "ID", "ENVIRONMENT", "DATA", "PROCEDURE", "DIVISION", "SECTION", "PROGRAM-ID", "FUNCTION-ID",
    "AUTHOR", "INSTALLATION", "DATE-WRITTEN", "DATE-COMPILED", "SECURITY", "CONFIGURATION", "SOURCE-COMPUTER",
    "OBJECT-COMPUTER", "SPECIAL-NAMES", "REPOSITORY", "INPUT-OUTPUT", "FILE-CONTROL", "I-O-CONTROL", "FILE",
    "WORKING-STORAGE", "LOCAL-STORAGE", "LINKAGE", "SCREEN", "REPORT", "COMMUNICATION", "DECLARATIVES", "END",
    "PROGRAM", "FUNCTION", "RECURSIVE", "INITIAL", "COMMON", "FD", "SD", "RD", "CD",
    // Data description clauses.
    "PIC", "PICTURE", "IS", "ARE", "USAGE", "VALUE", "VALUES", "OCCURS", "TIMES", "TO", "DEPENDING", "ON",
    "INDEXED", "BY", "ASCENDING", "DESCENDING", "KEY", "REDEFINES", "RENAMES", "THRU", "THROUGH", "FILLER",
    "JUSTIFIED", "JUST", "RIGHT", "LEFT", "BLANK", "WHEN", "ZERO", "ZEROS", "ZEROES", "SPACE", "SPACES", "HIGH-VALUE",
    "HIGH-VALUES", "LOW-VALUE", "LOW-VALUES", "QUOTE", "QUOTES", "NULL", "NULLS", "ALL", "SIGN", "LEADING",
    "TRAILING", "SEPARATE", "CHARACTER", "CHARACTERS", "SYNCHRONIZED", "SYNC", "EXTERNAL", "GLOBAL", "BASED",
    "ANY", "LENGTH", "CONSTANT", "AS", "FROM", "DISPLAY", "COMP", "COMP-1", "COMP-2", "COMP-3", "COMP-4", "COMP-5",
    "COMP-6", "COMP-X", "COMPUTATIONAL", "COMPUTATIONAL-1", "COMPUTATIONAL-2", "COMPUTATIONAL-3",
    "COMPUTATIONAL-4", "COMPUTATIONAL-5", "COMPUTATIONAL-X", "BINARY", "PACKED-DECIMAL", "INDEX", "POINTER",
    "PROGRAM-POINTER", "FUNCTION-POINTER", "BINARY-CHAR", "BINARY-SHORT", "BINARY-LONG", "BINARY-DOUBLE",
    "BINARY-C-LONG", "SIGNED", "UNSIGNED", "FLOAT-SHORT", "FLOAT-LONG", "FLOAT-DECIMAL-16", "FLOAT-DECIMAL-34",
    "NATIONAL", "ALPHANUMERIC", "NUMERIC", "ALPHABETIC", "ALPHABETIC-LOWER", "ALPHABETIC-UPPER", "POSITIVE",
    "NEGATIVE", "RECORD", "RECORDS", "CONTAINS", "LABEL", "STANDARD", "OMITTED", "BLOCK", "ASSIGN", "ORGANIZATION",
    "SEQUENTIAL", "LINE", "RELATIVE", "INDEXED", "ACCESS", "MODE", "RANDOM", "DYNAMIC", "STATUS", "SELECT",
    "OPTIONAL", "ALTERNATE", "DUPLICATES", "COPY", "REPLACE", "REPLACING", "OF", "IN", "TYPEDEF", "LOCALE",
    "DECIMAL-POINT", "COMMA", "CURRENCY", "CLASS", "SYMBOLIC", "CRT", "CURSOR", "CONSOLE", "SYSIN", "SYSOUT",
    "SYSERR", "ARGUMENT-NUMBER", "ARGUMENT-VALUE", "ENVIRONMENT-NAME", "ENVIRONMENT-VALUE", "COMMAND-LINE",
    // Statements.
    "ACCEPT", "ADD", "ALLOCATE", "ALTER", "CALL", "CANCEL", "CLOSE", "COMPUTE", "CONTINUE", "DELETE", "DIVIDE",
    "ENTRY", "EVALUATE", "EXIT", "FREE", "GENERATE", "GO", "GOBACK", "IF", "THEN", "ELSE", "INITIALIZE", "INSPECT",
    "INVOKE", "MERGE", "MOVE", "MULTIPLY", "OPEN", "PERFORM", "READ", "RELEASE", "RETURN", "REWRITE", "SEARCH",
    "SET", "SORT", "START", "STOP", "RUN", "STRING", "SUBTRACT", "UNSTRING", "USE", "WRITE", "RAISE", "RESUME",
    "END-ACCEPT", "END-ADD", "END-CALL", "END-COMPUTE", "END-DELETE", "END-DISPLAY", "END-DIVIDE", "END-EVALUATE",
    "END-IF", "END-MULTIPLY", "END-PERFORM", "END-READ", "END-RETURN", "END-REWRITE", "END-SEARCH", "END-START",
    "END-STRING", "END-SUBTRACT", "END-UNSTRING", "END-WRITE", "USING", "RETURNING", "GIVING", "INTO", "REMAINDER",
    "ROUNDED", "SIZE", "ERROR", "EXCEPTION", "OVERFLOW", "NOT", "AND", "OR", "UNTIL", "VARYING", "AFTER", "BEFORE",
    "TEST", "WITH", "NO", "ADVANCING", "UPON", "DELIMITED", "COUNT", "TALLYING", "CONVERTING", "FOR", "FIRST",
    "INPUT", "OUTPUT", "I-O", "EXTEND", "AT", "EQUAL", "EQUALS", "GREATER", "LESS", "THAN", "OTHER", "ALSO", "TRUE",
    "FALSE", "REFERENCE", "CONTENT", "ENTRY-CONVENTION", "STATIC", "ADDRESS", "NEXT", "SENTENCE", "INVALID",
    "CORRESPONDING", "CORR", "INITIALIZED", "UP", "DOWN", "RAISING", "TALLY", "RETURN-CODE", "EXCEPTION-OBJECT",
    "SELF", "SUPER", "WHEN-COMPILED", "LINES", "PAGE", "PROCEED", "INDICATE", "DEBUGGING", "TRACE", "READY",
    "RESET", "LOCK", "UNLOCK", "COMMIT", "ROLLBACK", "WAIT", "EOP", "END-OF-PAGE",
};

const std::unordered_set<std::string> &KeywordSet() {
    static const std::unordered_set<std::string> set(std::begin(kKeywords), std::end(kKeywords));
    return set;
}

bool AllDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string Trimmed(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

// FREE / FIXED named by a `>>SOURCE` or `$SET SOURCEFORMAT` directive on this line, else -1.
int DirectiveFormat(const std::string &line) {
    const std::string up = CobolUpper(line);
    size_t at = up.find(">>SOURCE");
    if (at == std::string::npos) {
        at = up.find("SOURCEFORMAT");
        if (at == std::string::npos || up.find("$SET") == std::string::npos) return -1;
    }
    const std::string rest = up.substr(at);
    if (rest.find("FREE") != std::string::npos) return static_cast<int>(CobolFormat::Free);
    if (rest.find("FIXED") != std::string::npos) return static_cast<int>(CobolFormat::Fixed);
    return -1;
}

// Does this line look like free-format code -- code inside what fixed format calls the sequence area?
bool LooksFree(const std::string &line) {
    const std::string t = Trimmed(line);
    if (t.empty() || t.compare(0, 2, "*>") == 0 || t.compare(0, 2, ">>") == 0) return false;
    const std::string area = line.substr(0, std::min<size_t>(6, line.size()));
    const std::string seq = Trimmed(area);
    if (seq.empty()) return false;
    if (line.find('\t') < 6) return false;
    // Six characters filling the area, then an indicator: a fixed-format line with a word where its
    // sequence number would be (GnuCOBOL ignores the area, whatever it holds).
    if (area.size() == 6 && area.find(' ') == std::string::npos &&
        (line.size() == 6 || std::strchr(" */-Dd", line[6]) != nullptr)) {
        return false;
    }
    return !AllDigits(seq);  // a sequence number is fixed format's; anything else there is code
}

// Scans [from, line end) for `*>` outside a literal; returns its column or -1.
int FloatingComment(const std::string &line, int from) {
    char quote = 0;
    for (int i = from; i < static_cast<int>(line.size()); i++) {
        const char c = line[static_cast<size_t>(i)];
        if (quote) {
            if (c == quote) quote = 0;  // a doubled quote reopens on the next character: same effect
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '*' && i + 1 < static_cast<int>(line.size()) && line[static_cast<size_t>(i) + 1] == '>') {
            return i;
        }
    }
    return -1;
}

}  // namespace

std::string CobolUpper(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool CobolIsKeyword(const std::string &word) { return KeywordSet().count(CobolUpper(word)) > 0; }

const std::vector<std::string> &CobolKeywords() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> v(KeywordSet().begin(), KeywordSet().end());
        std::sort(v.begin(), v.end());
        return v;
    }();
    return list;
}

std::vector<CobolFormat> CobolLineFormats(const std::vector<std::string> &lines) {
    CobolFormat guess = CobolFormat::Fixed;
    for (const std::string &l : lines) {
        if (DirectiveFormat(l) >= 0) break;  // the file says what it is from here on
        if (LooksFree(l)) {
            guess = CobolFormat::Free;
            break;
        }
    }
    std::vector<CobolFormat> out;
    out.reserve(lines.size());
    CobolFormat cur = guess;
    for (const std::string &l : lines) {
        out.push_back(cur);
        const int d = DirectiveFormat(l);
        if (d >= 0) cur = static_cast<CobolFormat>(d);
    }
    return out;
}

CobolLineInfo CobolClassifyLine(const std::string &line, CobolFormat format) {
    CobolLineInfo info;
    const int len = static_cast<int>(line.size());
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 2, ">>") == 0) {
        // A directive reads the same in either format (and is often what switches them).
        info.directive = true;
        info.code_start = static_cast<int>(first);
        info.comment_start = FloatingComment(line, info.code_start);
        info.code_end = info.comment_start >= 0 ? info.comment_start : len;
        return info;
    }
    if (format == CobolFormat::Fixed) {
        if (len <= 6) {
            info.code_start = info.code_end = len;
            return info;
        }
        const char ind = line[6];
        if (ind == '*' || ind == '/') {
            info.comment_line = true;
            info.comment_start = 6;
            info.code_start = info.code_end = 6;
            return info;
        }
        info.continuation = ind == '-';
        // `>>` / `$` directives may start in the indicator column.
        info.code_start = (ind == '>' || ind == '$') ? 6 : 7;
    } else {
        info.code_start = 0;
    }
    info.comment_start = FloatingComment(line, info.code_start);
    info.code_end = info.comment_start >= 0 ? info.comment_start : len;
    const std::string code = Trimmed(line.substr(static_cast<size_t>(info.code_start),
                                                 static_cast<size_t>(info.code_end - info.code_start)));
    if (code.empty() && info.comment_start >= 0) info.comment_line = true;
    if (code.compare(0, 2, ">>") == 0 || (code.size() > 1 && code[0] == '$' && std::isalpha(static_cast<unsigned char>(code[1])))) {
        info.directive = true;
    }
    return info;
}

bool CobolDocCommentText(const std::string &line, std::string *text) {
    size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos) return false;
    if (line.compare(at, 3, "*>>") != 0) {
        // Behind a sequence number: `000100*>> text`.
        if (line.size() >= 9 && line.compare(6, 3, "*>>") == 0) {
            at = 6;
        } else {
            return false;
        }
    }
    std::string rest = line.substr(at + 3);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r')) rest.pop_back();
    if (text) *text = rest;
    return true;
}
