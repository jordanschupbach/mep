// mep's COBOL formatter: see cobol_format.h.
//
// Three passes over the lines, none of which changes how many there are:
// read each line (its format, what kind of line it is, its tokens, with
// literals and pseudo-text carried across lines), walk them in order
// keeping the division, the data division's level hierarchy and the
// procedure division's open scopes to decide each line's column, then
// write each line at that column -- or as it was, when it is one of the
// lines that are never touched, or would not fit -- and check that what
// changed is only whitespace outside literals.
#include "cobol_format.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "cobol_syntax.h"

namespace cobolfmt {
namespace {

constexpr int kFixedCodeStart = 7;  // column 8: Area A
constexpr int kFixedCodeEnd = 72;   // one past column 72, where the identification area starts
constexpr int kFixedAreaB = 11;     // column 12
constexpr int kFreeMaxLine = 255;   // GnuCOBOL's free-format line limit
constexpr int kPicOffset = 32;      // PICTURE at Area A + 32: column 40 in fixed format

enum class TokKind { Word, Literal, OpenLiteral, Pseudo, Period, Other };

struct Token {
    TokKind kind = TokKind::Other;
    int start = 0;
    int end = 0;
    std::string up;  // the text, upper-cased (words only)
};

enum class LineKind { Blank, Comment, Directive, Debug, Continuation, Code };

struct Line {
    std::string text;  // without its line break or a trailing \r
    bool cr = false;
    CobolFormat format = CobolFormat::Fixed;
    LineKind kind = LineKind::Code;
    int code_end = 0;  // one past the code region (fixed: at most column 72)
    int first = -1;    // the code's first column, -1 for none
    std::vector<Token> tokens;
    bool keep = false;          // written exactly as it was
    bool open_literal = false;  // ends inside a literal (continued on the next line)
    int col = -1;               // where the code goes; -1: unchanged
    bool pic = false;           // `level name PIC ...`: the PICTURE clause may be aligned
};

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

const std::unordered_set<std::string> &Set(std::initializer_list<const char *> words, std::unordered_set<std::string> &s) {
    for (const char *w : words) s.insert(w);
    return s;
}

#define WORD_SET(name, ...)                                     \
    bool name(const std::string &w) {                           \
        static std::unordered_set<std::string> s;               \
        static const auto &set = Set({__VA_ARGS__}, s);          \
        return set.count(w) > 0;                                \
    }

// Statement verbs: a line starting with one starts a statement.
WORD_SET(IsVerb, "ACCEPT", "ADD", "ALLOCATE", "ALTER", "CALL", "CANCEL", "CHAIN", "CLOSE", "COMMIT", "COMPUTE",
         "CONTINUE", "DELETE", "DESTROY", "DISABLE", "DISPLAY", "DIVIDE", "ENABLE", "ENTRY", "EVALUATE", "EXHIBIT",
         "EXIT", "FREE", "GENERATE", "GO", "GOBACK", "IF", "INITIALIZE", "INITIATE", "INSPECT", "INVOKE", "JSON",
         "MERGE", "MOVE", "MULTIPLY", "NEXT", "OPEN", "PERFORM", "PURGE", "RAISE", "READ", "READY", "RECEIVE",
         "RELEASE", "RESET", "RESUME", "RETURN", "REWRITE", "ROLLBACK", "SEARCH", "SEND", "SET", "SORT", "START",
         "STOP", "STRING", "SUBTRACT", "SUPPRESS", "TERMINATE", "TRANSFORM", "UNLOCK", "UNSTRING", "USE", "VALIDATE",
         "WRITE", "XML")
// The verbs whose statements have an END-<verb> terminator (and so may have conditional phrases).
WORD_SET(HasEnd, "ACCEPT", "ADD", "CALL", "COMPUTE", "DELETE", "DISPLAY", "DIVIDE", "EVALUATE", "MULTIPLY", "READ",
         "RETURN", "REWRITE", "SEARCH", "START", "STRING", "SUBTRACT", "UNSTRING", "WRITE", "JSON", "XML")
WORD_SET(IsIdParagraph, "PROGRAM-ID", "FUNCTION-ID", "CLASS-ID", "METHOD-ID", "INTERFACE-ID", "FACTORY", "OBJECT",
         "OPTIONS", "AUTHOR", "INSTALLATION", "DATE-WRITTEN", "DATE-COMPILED", "SECURITY", "REMARKS")
WORD_SET(IsCommentEntryParagraph, "AUTHOR", "INSTALLATION", "DATE-WRITTEN", "DATE-COMPILED", "SECURITY", "REMARKS")
WORD_SET(IsEnvParagraph, "SOURCE-COMPUTER", "OBJECT-COMPUTER", "SPECIAL-NAMES", "REPOSITORY", "FILE-CONTROL",
         "I-O-CONTROL")
WORD_SET(IsEnvSection, "CONFIGURATION", "INPUT-OUTPUT")
WORD_SET(IsDataSection, "FILE", "WORKING-STORAGE", "LOCAL-STORAGE", "LINKAGE", "REPORT", "SCREEN", "COMMUNICATION")
WORD_SET(IsFileDescription, "FD", "SD", "RD", "CD")
WORD_SET(IsEndable, "PROGRAM", "FUNCTION", "METHOD", "CLASS", "FACTORY", "OBJECT", "INTERFACE")

// What can stand after `END` for it to be the AT END phrase rather than END PROGRAM and the like.
bool EndsUnit(const std::string &w) { return IsEndable(w) || w == "DECLARATIVES"; }

bool AllDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reading lines
// ---------------------------------------------------------------------------

struct ScanState {
    char quote = 0;       // inside a literal opened with this quote
    bool pseudo = false;  // inside ==pseudo-text==
};

// The tokens of s[begin, end), up to a `*>` comment.
void Tokenize(const std::string &s, int begin, int end, ScanState &st, std::vector<Token> &out) {
    int i = begin;
    auto literal_from = [&](int start, int body, char q) {
        int j = body;
        while (j < end) {
            if (s[static_cast<size_t>(j)] == q) {
                if (j + 1 < end && s[static_cast<size_t>(j) + 1] == q) {
                    j += 2;
                    continue;
                }
                out.push_back({TokKind::Literal, start, j + 1, ""});
                st.quote = 0;
                return j + 1;
            }
            j++;
        }
        out.push_back({TokKind::OpenLiteral, start, end, ""});
        st.quote = q;
        return end;
    };
    if (st.pseudo) {
        const size_t close = s.find("==", static_cast<size_t>(i));
        if (close == std::string::npos || static_cast<int>(close) + 2 > end) {
            out.push_back({TokKind::Pseudo, i, end, ""});
            return;
        }
        out.push_back({TokKind::Pseudo, i, static_cast<int>(close) + 2, ""});
        st.pseudo = false;
        i = static_cast<int>(close) + 2;
    }
    if (st.quote) {
        // A continued literal goes on from the continuation line's first quote.
        int q = i;
        while (q < end && s[static_cast<size_t>(q)] != st.quote) q++;
        if (q < end) {
            i = literal_from(q, q + 1, st.quote);
        } else {
            st.quote = 0;
        }
    }
    while (i < end) {
        const char c = s[static_cast<size_t>(i)];
        if (IsSpace(c)) {
            i++;
            continue;
        }
        if (c == '*' && i + 1 < end && s[static_cast<size_t>(i) + 1] == '>') break;
        if (c == '"' || c == '\'') {
            i = literal_from(i, i + 1, c);
            continue;
        }
        if (c == '=' && i + 1 < end && s[static_cast<size_t>(i) + 1] == '=') {
            const size_t close = s.find("==", static_cast<size_t>(i) + 2);
            if (close == std::string::npos || static_cast<int>(close) + 2 > end) {
                out.push_back({TokKind::Pseudo, i, end, ""});
                st.pseudo = true;
                return;
            }
            out.push_back({TokKind::Pseudo, i, static_cast<int>(close) + 2, ""});
            i = static_cast<int>(close) + 2;
            continue;
        }
        if (IsWordChar(c) || (c == '.' && i + 1 < end && std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(i) + 1])))) {
            int j = i + 1;
            while (j < end) {
                const char d = s[static_cast<size_t>(j)];
                if (IsWordChar(d)) {
                    j++;
                } else if ((d == '.' || d == ',') && j + 1 < end &&
                           std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(j) - 1])) &&
                           std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(j) + 1]))) {
                    j++;  // a decimal point inside a number
                } else {
                    break;
                }
            }
            // X"..", N"..", Z"..": a prefixed literal.
            if (j < end && (s[static_cast<size_t>(j)] == '"' || s[static_cast<size_t>(j)] == '\'') && j - i <= 2) {
                bool letters = true;
                for (int k = i; k < j; k++) letters = letters && std::isalpha(static_cast<unsigned char>(s[static_cast<size_t>(k)]));
                if (letters) {
                    i = literal_from(i, j + 1, s[static_cast<size_t>(j)]);
                    continue;
                }
            }
            out.push_back({TokKind::Word, i, j, CobolUpper(s.substr(static_cast<size_t>(i), static_cast<size_t>(j - i)))});
            i = j;
            continue;
        }
        if (c == '.' && (i + 1 >= end || IsSpace(s[static_cast<size_t>(i) + 1]) || s[static_cast<size_t>(i) + 1] == '\r')) {
            out.push_back({TokKind::Period, i, i + 1, ""});
            i++;
            continue;
        }
        out.push_back({TokKind::Other, i, i + 1, std::string(1, c)});
        i++;
    }
}

std::vector<Line> ReadLines(std::string_view src, bool *final_newline) {
    std::vector<Line> lines;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t nl = src.find('\n', pos);
        if (nl == std::string_view::npos) nl = src.size();
        Line l;
        l.text = std::string(src.substr(pos, nl - pos));
        if (!l.text.empty() && l.text.back() == '\r') {
            l.text.pop_back();
            l.cr = true;
        }
        lines.push_back(std::move(l));
        pos = nl + 1;
    }
    *final_newline = !src.empty() && src.back() == '\n';
    std::vector<std::string> texts;
    texts.reserve(lines.size());
    for (const Line &l : lines) texts.push_back(l.text);
    const std::vector<CobolFormat> formats = CobolLineFormats(texts);

    ScanState st;
    for (size_t row = 0; row < lines.size(); row++) {
        Line &l = lines[row];
        l.format = formats[row];
        const int len = static_cast<int>(l.text.size());
        const size_t nonblank = l.text.find_first_not_of(" \t");
        const bool continuing = st.quote != 0;
        int begin = 0;
        if (nonblank == std::string::npos) {
            l.kind = LineKind::Blank;
        } else if (CobolClassifyLine(l.text, l.format).directive) {
            l.kind = LineKind::Directive;
        } else if (l.format == CobolFormat::Fixed) {
            const char ind = len > 6 ? l.text[6] : ' ';
            if (len <= 6 || l.text.find_first_not_of(' ', 6) == std::string::npos) {
                l.kind = LineKind::Blank;  // a sequence number and nothing else
            } else if (ind == '*' || ind == '/') {
                l.kind = LineKind::Comment;
            } else if (ind == 'D' || ind == 'd') {
                l.kind = LineKind::Debug;
            } else if (ind == '-') {
                l.kind = LineKind::Continuation;
                begin = kFixedCodeStart;
            } else {
                begin = kFixedCodeStart;
                if (ind != ' ') l.keep = true;  // an indicator GnuCOBOL does not know
            }
            l.code_end = std::min(len, kFixedCodeEnd);
        } else {
            const CobolLineInfo info = CobolClassifyLine(l.text, l.format);
            if (info.comment_line) {
                l.kind = LineKind::Comment;
            } else if (continuing && l.text[nonblank] == '-') {
                l.kind = LineKind::Continuation;
                begin = static_cast<int>(nonblank) + 1;
            }
            l.code_end = len;
        }
        if (l.kind != LineKind::Code && l.kind != LineKind::Continuation) {
            if (l.kind != LineKind::Blank) st = ScanState{};
            continue;
        }
        if (l.kind == LineKind::Code) st.quote = 0;  // a literal left open and not continued ends there
        const size_t code = l.text.find_first_not_of(" \t", static_cast<size_t>(begin));
        if (code == std::string::npos || static_cast<int>(code) >= l.code_end) {
            if (l.kind == LineKind::Code) l.kind = LineKind::Blank;
            continue;
        }
        l.first = static_cast<int>(code);
        // A tab before the code: its column depends on the tab stops, so the line is left alone.
        if (l.text.find('\t') < code) l.keep = true;
        Tokenize(l.text, begin, l.code_end, st, l.tokens);
        l.open_literal = st.quote != 0;
        if (l.open_literal) l.keep = true;
        // In fixed format a word or literal running on into the identification area.
        if (l.format == CobolFormat::Fixed && len > kFixedCodeEnd && !IsSpace(l.text[kFixedCodeEnd - 1]) &&
            !IsSpace(l.text[kFixedCodeEnd])) {
            l.keep = true;
        }
    }
    // The line a continuation line continues must stay as it is (in fixed format up to column 72).
    for (size_t row = 1; row < lines.size(); row++) {
        if (lines[row].kind != LineKind::Continuation) continue;
        for (size_t k = row; k-- > 0;) {
            if (lines[k].kind == LineKind::Code || lines[k].kind == LineKind::Continuation) {
                lines[k].keep = true;
                break;
            }
            if (lines[k].kind != LineKind::Comment && lines[k].kind != LineKind::Blank) break;
        }
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Deciding columns
// ---------------------------------------------------------------------------

struct Scope {
    enum Kind { If, Perform, Stmt } kind = Stmt;
    std::string verb;  // Stmt: its verb
    int depth = 0;
    bool phrase = false;  // Stmt: inside a conditional phrase (or WHEN) of it
};

enum class Phrase { None, AtEnd, EndOfPage, Invalid, SizeError, Exception, Overflow };

bool Accepts(const std::string &verb, Phrase p) {
    switch (p) {
        case Phrase::AtEnd:
            return verb == "READ" || verb == "RETURN" || verb == "SEARCH";
        case Phrase::EndOfPage:
            return verb == "WRITE";
        case Phrase::Invalid:
            return verb == "READ" || verb == "WRITE" || verb == "REWRITE" || verb == "DELETE" || verb == "START";
        case Phrase::SizeError:
            return verb == "ADD" || verb == "SUBTRACT" || verb == "MULTIPLY" || verb == "DIVIDE" || verb == "COMPUTE";
        case Phrase::Exception:
            return verb == "CALL" || verb == "JSON" || verb == "XML";
        case Phrase::Overflow:
            return verb == "CALL" || verb == "STRING" || verb == "UNSTRING";
        case Phrase::None:
            break;
    }
    return false;
}

std::string Rewrite(const Line &l, bool align);

class Walker {
public:
    Walker(std::vector<Line> &lines, const Options &opt) : lines_(lines), opt_(opt) {}

    void Run() {
        for (row_ = 0; row_ < lines_.size(); row_++) {
            Line &l = lines_[row_];
            if (l.kind == LineKind::Continuation) {
                // Part of the line before it: its tokens count (a period may follow the literal).
                ApplyFrom(l, 0);
                continue;
            }
            if (l.kind != LineKind::Code || l.tokens.empty()) continue;
            const int col = Place(l);
            if (col == -2) {
                // A continuation: where it was relative to the line it continues.
                const int offset = l.first - ref_orig_;
                const int min_offset = div_ == Div::Proc ? opt_.indent : 0;
                l.col = ref_new_ + std::max(offset, min_offset);
            } else {
                l.col = col;
            }
            // A line that stays where it is -- kept, or not fitting at its new column -- is
            // where its continuation lines are measured from.
            if (l.keep || (l.col >= 0 && l.col != l.first && Rewrite(l, false) == l.text)) l.col = -1;
            if (col != -2) {
                ref_orig_ = l.first;
                ref_new_ = l.col >= 0 ? l.col : l.first;
            }
        }
    }

private:
    enum class Div { None, Ident, Env, Data, Proc };
    enum class Skip { None, Copy, Replace, Exec };

    int AreaA(const Line &l) const { return l.format == CobolFormat::Fixed ? kFixedCodeStart : 0; }
    int AreaB(const Line &l) const { return l.format == CobolFormat::Fixed ? kFixedAreaB : opt_.indent; }
    int StatementCol(const Line &l, int depth) const { return AreaB(l) + depth * opt_.indent; }

    int Body() const {
        if (stack_.empty()) return 0;
        const Scope &s = stack_.back();
        if (s.kind != Scope::Stmt) return s.depth + 1;
        return s.phrase ? s.depth + 2 : s.depth;
    }

    void PopStale() {
        while (!stack_.empty() && stack_.back().kind == Scope::Stmt && !stack_.back().phrase) stack_.pop_back();
    }

    void Reset() {
        stack_.clear();
        levels_.clear();
        item_depth_ = 0;
        open_ = false;
        comment_entry_ = false;
    }

    static const std::string &Up(const Line &l, size_t i) {
        static const std::string none;
        return i < l.tokens.size() && l.tokens[i].kind == TokKind::Word ? l.tokens[i].up : none;
    }
    static bool IsPeriod(const Line &l, size_t i) { return i < l.tokens.size() && l.tokens[i].kind == TokKind::Period; }

    // A conditional phrase starting at token i, and how many tokens it takes.
    static Phrase PhraseAt(const Line &l, size_t i, size_t *len) {
        size_t k = i;
        if (Up(l, k) == "NOT") k++;
        const std::string &w = Up(l, k);
        Phrase p = Phrase::None;
        size_t n = 0;
        if (w == "AT" || w == "END" || w == "END-OF-PAGE" || w == "EOP") {
            size_t m = k;
            if (w == "AT") m++;
            const std::string &v = Up(l, m);
            if (v == "END" && !EndsUnit(Up(l, m + 1))) {
                p = Phrase::AtEnd;
                n = m + 1 - i;
            } else if (v == "END-OF-PAGE" || v == "EOP") {
                p = Phrase::EndOfPage;
                n = m + 1 - i;
            }
        } else if (w == "INVALID") {
            p = Phrase::Invalid;
            n = k + 1 - i + (Up(l, k + 1) == "KEY" ? 1 : 0);
        } else if (w == "ON" || w == "SIZE" || w == "EXCEPTION" || w == "OVERFLOW") {
            size_t m = k;
            if (w == "ON") m++;
            const std::string &v = Up(l, m);
            if (v == "SIZE" && Up(l, m + 1) == "ERROR") {
                p = Phrase::SizeError;
                n = m + 2 - i;
            } else if (v == "EXCEPTION") {
                p = Phrase::Exception;
                n = m + 1 - i;
            } else if (v == "OVERFLOW") {
                p = Phrase::Overflow;
                n = m + 1 - i;
            }
        }
        *len = n;
        return p;
    }

    // The innermost scope matching `match`, or -1.
    template <typename F>
    int Find(F match) const {
        for (size_t j = stack_.size(); j-- > 0;) {
            if (match(stack_[j])) return static_cast<int>(j);
        }
        return -1;
    }

    // Applies token i (and any it consumes) to the scopes. When `depth` is given, the depth the
    // line goes at if it starts with this token: the scopes it closes are closed first, those it
    // opens are opened after. -1 means the line continues a statement.
    void Apply(const Line &l, size_t &i, int *depth) {
        const Token &t = l.tokens[i];
        auto set = [depth](int d) {
            if (depth) *depth = d;
        };
        if (skip_ != Skip::None) {
            set(-1);
            if (skip_ == Skip::Exec) {
                if (t.up == "END-EXEC") skip_ = Skip::None;
            } else if (t.kind == TokKind::Period) {
                // COPY's period also ends the sentence it stands in; REPLACE's ends only itself.
                if (skip_ == Skip::Copy) EndSentence();
                skip_ = Skip::None;
            }
            i++;
            return;
        }
        if (t.kind == TokKind::Period) {
            set(0);
            EndSentence();
            i++;
            return;
        }
        const std::string &w = t.up;
        if (t.kind != TokKind::Word) {
            open_ = true;
            set(-1);
            i++;
            return;
        }
        if (w == "COPY") {
            skip_ = Skip::Copy;
            set(-1);
            i++;
            return;
        }
        if (w == "REPLACE") {
            skip_ = Skip::Replace;
            set(-1);
            i++;
            return;
        }
        if (w == "EXEC" || w == "EXECUTE") {
            skip_ = Skip::Exec;
            set(-1);
            i++;
            return;
        }
        open_ = true;
        if (div_ != Div::Proc) {
            if (div_ == Div::Data && (i == 0 || IsPeriod(l, i - 1) || after_period_)) {
                if (AllDigits(w) && w.size() <= 2) LevelDepth(std::stoi(w));
                if (IsFileDescription(w)) {
                    levels_.clear();
                    item_depth_ = 0;
                }
            }
            after_period_ = false;
            set(-1);
            i++;
            return;
        }
        if (w == "FUNCTION") {  // the function's name is no verb
            set(-1);
            i += 2;
            return;
        }
        if (w == "ELSE") {
            const int j = Find([](const Scope &s) { return s.kind == Scope::If; });
            if (j >= 0) {
                stack_.resize(static_cast<size_t>(j) + 1);
                set(stack_.back().depth);
                if (Up(l, i + 1) == "IF") {
                    // ELSE IF: the inner IF at the outer one's depth, an else-if chain.
                    stack_.push_back({Scope::If, "", stack_.back().depth, false});
                    i++;
                }
            } else {
                set(Body());
            }
            i++;
            return;
        }
        if (w.compare(0, 4, "END-") == 0 && w != "END-OF-PAGE") {
            const std::string verb = w.substr(4);
            int j = -1;
            if (verb == "IF") {
                j = Find([](const Scope &s) { return s.kind == Scope::If; });
            } else if (verb == "PERFORM") {
                j = Find([](const Scope &s) { return s.kind == Scope::Perform; });
            } else {
                j = Find([&verb](const Scope &s) { return s.kind == Scope::Stmt && s.verb == verb; });
            }
            if (j >= 0) {
                set(stack_[static_cast<size_t>(j)].depth);
                stack_.resize(static_cast<size_t>(j));
            } else {
                set(Body());
            }
            i++;
            return;
        }
        if (w == "WHEN") {
            const int j = Find([](const Scope &s) { return s.kind == Scope::Stmt && (s.verb == "EVALUATE" || s.verb == "SEARCH"); });
            if (j >= 0) {
                stack_.resize(static_cast<size_t>(j) + 1);
                stack_.back().phrase = true;
                set(stack_.back().depth + 1);
            } else {
                set(-1);
            }
            i++;
            return;
        }
        size_t len = 0;
        const Phrase p = PhraseAt(l, i, &len);
        if (p != Phrase::None) {
            const int j = Find([p](const Scope &s) { return s.kind == Scope::Stmt && Accepts(s.verb, p); });
            if (j >= 0) {
                stack_.resize(static_cast<size_t>(j) + 1);
                stack_.back().phrase = true;
                set(stack_.back().depth + 1);
                i += len;
                return;
            }
        }
        if (IsVerb(w)) {
            PopStale();
            const int d = Body();
            set(d);
            if (w == "IF") {
                stack_.push_back({Scope::If, "", d, false});
            } else if (w == "PERFORM") {
                if (InlinePerform(l, i)) stack_.push_back({Scope::Perform, "", d, false});
            } else if (w == "EXIT") {
                const std::string &n = Up(l, i + 1);
                if (n == "PERFORM" || n == "PARAGRAPH" || n == "SECTION" || n == "PROGRAM" || n == "METHOD" ||
                    n == "FUNCTION") {
                    i++;
                }
            } else if (w == "NEXT" && Up(l, i + 1) == "SENTENCE") {
                i++;
            } else if (HasEnd(w)) {
                stack_.push_back({Scope::Stmt, w, d, false});
            }
            i++;
            return;
        }
        set(-1);
        i++;
    }

    // Whether the PERFORM at token i is an inline one (a body and END-PERFORM follow) rather than a
    // call of a procedure.
    static bool InlinePerform(const Line &l, size_t i) {
        if (i + 1 >= l.tokens.size()) return true;
        const Token &n = l.tokens[i + 1];
        if (n.kind == TokKind::Period) return false;
        const std::string &w = n.up;
        if (w == "VARYING" || w == "UNTIL" || w == "WITH" || w == "TEST" || w == "FOREVER") return true;
        return Up(l, i + 2) == "TIMES";
    }

    void EndSentence() {
        stack_.clear();
        open_ = false;
        after_period_ = true;
    }

    // The depth of a data entry of level `n` in the record's hierarchy, -1 for no level number.
    int LevelDepth(int n) {
        if (n == 1) {
            levels_.assign(1, 1);
            item_depth_ = 0;
            return 0;
        }
        if (n >= 2 && n <= 49) {
            // A copybook of fields with no 01 of its own: they belong to a record around them.
            if (levels_.empty()) levels_.push_back(0);
            while (levels_.size() > 1 && levels_.back() >= n) levels_.pop_back();
            if (levels_.back() >= n) levels_.back() = 0;
            const int d = static_cast<int>(levels_.size());
            levels_.push_back(n);
            item_depth_ = d;
            return d;
        }
        if (n == 66) return 0;
        if (n == 77 || n == 78) {
            levels_.clear();
            item_depth_ = 0;
            return 0;
        }
        if (n == 88) return item_depth_ + 1;
        return -1;
    }

    // Applies the tokens from i on.
    void ApplyFrom(const Line &l, size_t i) {
        while (i < l.tokens.size()) Apply(l, i, nullptr);
    }

    // Skips a header's tokens up to and including its period, then applies the rest.
    void AfterHeader(const Line &l) {
        size_t i = 0;
        while (i < l.tokens.size() && l.tokens[i].kind != TokKind::Period) i++;
        if (i < l.tokens.size()) {
            EndSentence();
            i++;
        } else {
            open_ = true;
        }
        ApplyFrom(l, i);
    }

    // The column of line l's code: >= 0, -1 for as it is, -2 for a continuation line.
    int Place(Line &l) {
        const std::string &w0 = Up(l, 0);
        // The second word, from the next line when this one has only one: `DATA` / `DIVISION.`.
        const std::string &w1 = l.tokens.size() == 1 ? NextLineWord() : Up(l, 1);
        const bool at_start = !open_ && skip_ == Skip::None;
        if (skip_ != Skip::None) {
            ApplyFrom(l, 0);
            return -1;
        }
        if (w0 == "COPY" || w0 == "REPLACE" || w0 == "EXEC" || w0 == "EXECUTE") {
            ApplyFrom(l, 0);
            return -1;
        }
        // Division headers and END PROGRAM, wherever they are.
        if (w1 == "DIVISION" && (w0 == "IDENTIFICATION" || w0 == "ID" || w0 == "ENVIRONMENT" || w0 == "DATA" ||
                                 w0 == "PROCEDURE")) {
            // A header in Area B inside the procedure division (no END PROGRAM before it) may be
            // read as part of the procedure division where Area A is checked: left where it is.
            const bool stay = div_ == Div::Proc && l.format == CobolFormat::Fixed && l.first > kFixedAreaB - 1;
            Reset();
            div_ = w0 == "ENVIRONMENT" ? Div::Env : w0 == "DATA" ? Div::Data : w0 == "PROCEDURE" ? Div::Proc : Div::Ident;
            AfterHeader(l);
            return stay ? -1 : AreaA(l);
        }
        if (w0 == "END" && IsEndable(w1)) {
            Reset();
            div_ = Div::None;
            AfterHeader(l);
            return AreaA(l);
        }
        if (div_ == Div::None) {
            if (IsIdParagraph(w0)) {
                div_ = Div::Ident;
            } else if (at_start && AllDigits(w0) && w0.size() <= 2) {
                div_ = Div::Data;  // a copybook of data entries
            } else if (IsVerb(w0)) {
                div_ = Div::Proc;  // a copybook of statements
            } else {
                ApplyFrom(l, 0);
                return -1;
            }
        }
        switch (div_) {
            case Div::Ident:
                if (IsIdParagraph(w0) && IsPeriod(l, 1)) {
                    comment_entry_ = IsCommentEntryParagraph(w0);
                    if (comment_entry_) {
                        open_ = false;
                    } else {
                        AfterHeader(l);
                    }
                    return AreaA(l);
                }
                if (!comment_entry_) ApplyFrom(l, 0);
                return -1;
            case Div::Env: {
                if ((IsEnvSection(w0) && w1 == "SECTION") || (IsEnvParagraph(w0) && IsPeriod(l, 1))) {
                    AfterHeader(l);
                    return AreaA(l);
                }
                ApplyFrom(l, 0);
                return at_start ? AreaB(l) : -2;
            }
            case Div::Data: {
                if (IsDataSection(w0) && w1 == "SECTION") {
                    AfterHeader(l);
                    levels_.clear();
                    item_depth_ = 0;
                    return AreaA(l);
                }
                if (!at_start) {
                    ApplyFrom(l, 0);
                    return -2;
                }
                if (IsFileDescription(w0)) {
                    ApplyFrom(l, 0);
                    return AreaA(l);
                }
                if (AllDigits(w0) && w0.size() <= 2) {
                    const int d = LevelDepth(std::stoi(w0));
                    size_t i = 1;
                    open_ = true;
                    after_period_ = false;
                    ApplyFrom(l, i);
                    if (d < 0) return -1;
                    const std::string &name = Up(l, 1);
                    l.pic = l.tokens.size() > 2 && l.tokens[1].kind == TokKind::Word &&
                            (name == "FILLER" || !CobolIsKeyword(name)) &&
                            (Up(l, 2) == "PIC" || Up(l, 2) == "PICTURE");
                    return AreaA(l) + d * opt_.indent;
                }
                ApplyFrom(l, 0);
                return -1;
            }
            case Div::Proc: {
                if (at_start && l.tokens[0].kind == TokKind::Word) {
                    const bool section = w1 == "SECTION";
                    const bool paragraph = IsPeriod(l, 1) && !IsVerb(w0) && !CobolIsKeyword(w0);
                    if (section || paragraph || (w0 == "DECLARATIVES" && IsPeriod(l, 1)) ||
                        (w0 == "END" && w1 == "DECLARATIVES")) {
                        AfterHeader(l);
                        return AreaA(l);
                    }
                }
                int depth = -1;
                size_t i = 0;
                const bool was_open = open_;
                Apply(l, i, &depth);
                if (depth == -1 && !was_open) {
                    // Not a verb this formatter knows, at the start of a sentence: a statement.
                    PopStale();
                    depth = Body();
                }
                ApplyFrom(l, i);
                return depth < 0 ? -2 : StatementCol(l, depth);
            }
            case Div::None:
                break;
        }
        return -1;
    }

    // The first word of the next code line ("" when there is none).
    const std::string &NextLineWord() const {
        static const std::string none;
        for (size_t k = row_ + 1; k < lines_.size(); k++) {
            const Line &n = lines_[k];
            if (n.kind == LineKind::Code) return n.tokens.empty() ? none : Up(n, 0);
            if (n.kind != LineKind::Blank && n.kind != LineKind::Comment) break;
        }
        return none;
    }

    std::vector<Line> &lines_;
    const Options &opt_;
    size_t row_ = 0;
    Div div_ = Div::None;
    Skip skip_ = Skip::None;
    bool open_ = false;           // a sentence (an entry, a statement) is going on
    bool after_period_ = false;   // the last token was a period
    bool comment_entry_ = false;  // inside AUTHOR. and the like
    int ref_orig_ = 0;            // the last line that was not a continuation: where it was,
    int ref_new_ = 0;             // and where it goes
    std::vector<int> levels_;     // the data division's open level numbers
    int item_depth_ = 0;          // the depth of the last entry an 88 would belong to
    std::vector<Scope> stack_;    // the procedure division's open scopes
};

// ---------------------------------------------------------------------------
// Writing lines
// ---------------------------------------------------------------------------

// The text with whitespace outside literals collapsed to single spaces and trimmed.
std::string Normalize(const std::string &s) {
    std::string out;
    char quote = 0;
    bool space = false;
    for (char c : s) {
        if (quote) {
            out += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (IsSpace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        if (c == '"' || c == '\'') quote = c;
        out += c;
    }
    return out;
}

std::string RTrim(std::string s) {
    while (!s.empty() && IsSpace(s.back())) s.pop_back();
    return s;
}

// Line l's code written at column l.col, or the line as it was when it does not fit.
std::string Rewrite(const Line &l, bool align) {
    const bool fixed = l.format == CobolFormat::Fixed;
    const int area_end = fixed ? std::min(static_cast<int>(l.text.size()), kFixedCodeEnd) : static_cast<int>(l.text.size());
    std::string content = RTrim(l.text.substr(static_cast<size_t>(l.first), static_cast<size_t>(area_end - l.first)));
    if (align && l.pic) {
        const Token &name = l.tokens[1];
        const Token &pic = l.tokens[2];
        const int area_a = fixed ? kFixedCodeStart : 0;
        const int name_end = l.col + (name.end - l.first);
        const int target = area_a + kPicOffset;
        if (name_end < target) {
            content = content.substr(0, static_cast<size_t>(name.end - l.first)) +
                      std::string(static_cast<size_t>(target - name_end), ' ') +
                      content.substr(static_cast<size_t>(pic.start - l.first));
        }
    }
    if (fixed) {
        if (l.col < kFixedCodeStart || l.col + static_cast<int>(content.size()) > kFixedCodeEnd) return l.text;
        std::string out = l.text.substr(0, kFixedCodeStart);
        out += std::string(static_cast<size_t>(l.col - kFixedCodeStart), ' ');
        out += content;
        if (static_cast<int>(l.text.size()) > kFixedCodeEnd) {
            out += std::string(static_cast<size_t>(kFixedCodeEnd - static_cast<int>(out.size())), ' ');
            out += l.text.substr(kFixedCodeEnd);
        }
        return out;
    }
    if (l.col + static_cast<int>(content.size()) > kFreeMaxLine) return l.text;
    return std::string(static_cast<size_t>(l.col), ' ') + content;
}

// Whether `out` says what `in` said: the same sequence area, indicator and identification area,
// and the same code up to whitespace outside literals.
bool SameMeaning(const Line &l, const std::string &out) {
    if (l.format == CobolFormat::Fixed) {
        if (out.compare(0, kFixedCodeStart, l.text, 0, kFixedCodeStart) != 0) return false;
        const std::string in_id = l.text.size() > kFixedCodeEnd ? l.text.substr(kFixedCodeEnd) : "";
        const std::string out_id = out.size() > kFixedCodeEnd ? out.substr(kFixedCodeEnd) : "";
        if (in_id != out_id) return false;
        auto code = [](const std::string &s) {
            return s.size() > kFixedCodeStart ? s.substr(kFixedCodeStart, kFixedCodeEnd - kFixedCodeStart) : "";
        };
        return Normalize(code(l.text)) == Normalize(code(out));
    }
    return Normalize(l.text) == Normalize(out);
}

}  // namespace

Result Format(std::string_view src, const Options &opts_in) {
    Result r;
    r.text = std::string(src);
    Options opts = opts_in;
    if (opts.indent < 1 || opts.indent > 16) opts.indent = 4;
    bool final_newline = false;
    std::vector<Line> lines = ReadLines(src, &final_newline);

    // A guess of free format in a file that also has fixed-format sequence numbers is no guess to
    // re-indent on: moving code into columns 1-7 would break it if it is fixed after all.
    {
        bool guessed_free = false;
        int numbered = 0;
        for (const Line &l : lines) {
            const std::string up = CobolUpper(l.text);
            if (up.find(">>SOURCE") != std::string::npos || up.find("SOURCEFORMAT") != std::string::npos) break;
            if (l.format == CobolFormat::Free && l.kind != LineKind::Blank) guessed_free = true;
            if (l.text.size() >= 7 && AllDigits(l.text.substr(0, 6)) && (l.text[6] == ' ' || l.text[6] == '*')) {
                numbered++;
                if (guessed_free && numbered == 1) r.error_line = static_cast<int>(&l - lines.data()) + 1;
            }
        }
        if (guessed_free && numbered > 0) {
            r.error = "the reference format is unclear (free-format code, but fixed-format sequence numbers)";
            if (r.error_line == 0) r.error_line = 1;
            return r;
        }
    }

    Walker(lines, opts).Run();

    std::string out;
    out.reserve(src.size() + src.size() / 8);
    for (size_t row = 0; row < lines.size(); row++) {
        const Line &l = lines[row];
        std::string text = l.text;
        if (l.kind == LineKind::Code && !l.keep && l.col >= 0 && l.first >= 0) {
            text = Rewrite(l, opts.align_pic);
            if (text == l.text && l.pic && opts.align_pic) text = Rewrite(l, false);
            if (text != l.text && !SameMeaning(l, text)) {
                r.error = "formatting would change the code";
                r.error_line = static_cast<int>(row) + 1;
                r.text = std::string(src);
                return r;
            }
        }
        out += text;
        if (l.cr) out += '\r';
        if (row + 1 < lines.size() || final_newline) out += '\n';
    }
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace cobolfmt
