// Bash parser for mep's own Bash language server: see bash_parse.h.
//
// Recursive descent over the raw text, with no separate tokenizer: Bash's
// lexer depends on the parser's state at every turn (reserved words only
// in command position, `<` a redirection or a string comparison, `((` an
// arithmetic command or two subshells, here-document bodies starting at the
// next newline), so each parse function reads exactly the characters its
// construct allows. The grammar is parse.y's: lists of and-or lists of
// pipelines of commands, the compound commands, function definitions, and
// the word-level constructs (quoting, the expansions, extglob).
//
// A backslash-newline is a line continuation everywhere but inside single
// quotes, ANSI-C quotes, comments and here-document bodies; Norm() steps
// over it, so the rest of the parser never sees one.
#include "bash_parse.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace {

constexpr int kMaxDepth = 1000;

struct ParseFailure {
    size_t pos;
    size_t end;
    std::string code;
    std::string message;
};

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

bool IsMeta(char c) {
    return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '(' || c == ')' ||
           c == '<' || c == '>';
}

bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsDeclarationBuiltin(const std::string &w) {
    return w == "declare" || w == "typeset" || w == "local" || w == "export" || w == "readonly";
}

bool IsCloser(const std::string &w) {
    return w == "then" || w == "else" || w == "elif" || w == "fi" || w == "do" || w == "done" || w == "esac" ||
           w == "}" || w == "]]";
}

class Parser {
public:
    explicit Parser(const std::vector<std::string> &lines) {
        for (size_t i = 0; i < lines.size(); i++) {
            line_starts_.push_back(s_.size());
            s_ += lines[i];
            if (i + 1 < lines.size()) s_ += '\n';
        }
        if (line_starts_.empty()) line_starts_.push_back(0);
        n_ = s_.size();
        end_ = n_;
    }

    BashParseResult Run() {
        for (;;) {
            try {
                ParseTopLevel();
                break;
            } catch (const ParseFailure &f) {
                if (!res_.has_syntax_error) {
                    res_.has_syntax_error = true;
                    BashParseError e;
                    e.range = Range(f.pos, std::max(f.end, f.pos));
                    e.code = f.code;
                    e.message = f.message;
                    res_.errors.push_back(e);
                }
                recovering_ = true;
                end_ = n_;
                depth_ = 0;
                pending_.clear();
                const size_t from = std::min(std::max(i_, f.pos), n_);
                const size_t nl = s_.find('\n', from);
                if (nl == std::string::npos) break;
                i_ = nl + 1;
            }
        }
        res_.program.range = Range(0, n_);
        FixHeredocs(res_.program);
        return std::move(res_);
    }

private:
    std::string s_;
    size_t n_ = 0;
    size_t end_ = 0;
    size_t i_ = 0;
    std::vector<size_t> line_starts_;
    BashParseResult res_;
    bool recovering_ = false;
    int depth_ = 0;

    struct PendingHeredoc {
        int id;
        std::string delimiter;
        bool quoted;
        bool strip_tabs;
        size_t op_pos;
    };
    std::vector<PendingHeredoc> pending_;
    // The compound commands open around the cursor, innermost last, for
    // bash's "unexpected end of file from `if' command on line 3".
    std::vector<std::pair<std::string, int>> open_;
    struct OpenGuard {
        Parser *p;
        OpenGuard(Parser *parser, const std::string &kw, size_t at) : p(parser) {
            int line = 0, col = 0;
            p->LineCol(at, &line, &col);
            p->open_.emplace_back(kw, line + 1);
        }
        ~OpenGuard() { p->open_.pop_back(); }
    };
    struct HeredocResult {
        BashRange body;
        std::vector<BashWordPart> parts;
        bool unterminated = false;
    };
    std::unordered_map<int, HeredocResult> heredocs_;
    int next_heredoc_ = 0;

    struct DepthGuard {
        Parser *p;
        explicit DepthGuard(Parser *parser) : p(parser) {
            if (++p->depth_ > kMaxDepth) {
                p->depth_ = 0;
                throw ParseFailure{p->i_, p->i_ + 1, "syntax-error", "nesting too deep"};
            }
        }
        ~DepthGuard() {
            if (p->depth_ > 0) p->depth_--;
        }
    };

    // --- Positions -----------------------------------------------------

    void LineCol(size_t pos, int *line, int *col) const {
        const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), pos);
        const size_t li = static_cast<size_t>(it - line_starts_.begin()) - 1;
        *line = static_cast<int>(li);
        *col = static_cast<int>(pos - line_starts_[li]);
    }

    BashRange Range(size_t a, size_t b) const {
        BashRange r;
        LineCol(a, &r.line, &r.col);
        LineCol(b, &r.end_line, &r.end_col);
        return r;
    }

    // --- Characters ----------------------------------------------------

    // Steps over line continuations at `p`.
    size_t Norm(size_t p) const {
        while (p + 1 < end_ && s_[p] == '\\' && s_[p + 1] == '\n') p += 2;
        return p;
    }
    char At(size_t p) const { return p < end_ ? s_[p] : '\0'; }
    char C() {
        i_ = Norm(i_);
        return At(i_);
    }
    char Ck(int k) {
        size_t p = Norm(i_);
        for (int j = 0; j < k; j++) {
            if (p >= end_) return '\0';
            p = Norm(p + 1);
        }
        return At(p);
    }
    char C1() { return Ck(1); }
    void Adv() {
        i_ = Norm(i_);
        if (i_ < end_) i_ = Norm(i_ + 1);
    }
    void AdvN(size_t k) {
        for (size_t j = 0; j < k; j++) Adv();
    }
    bool AtEnd() { return Norm(i_) >= end_; }
    size_t Here() { return Norm(i_); }

    void SkipBlanks() {
        while (IsBlank(C())) Adv();
    }

    void SkipComment() {
        const size_t start = Here();
        size_t p = start;
        while (p < end_ && s_[p] != '\n') p++;
        BashComment c;
        c.range = Range(start, p);
        c.text = s_.substr(start, p - start);
        res_.comments.push_back(c);
        i_ = p;
    }

    void SkipBlanksAndComment() {
        SkipBlanks();
        if (C() == '#') SkipComment();
    }

    void ConsumeNewline() {
        i_ = Norm(i_);
        if (i_ < end_ && s_[i_] == '\n') i_++;
        if (!pending_.empty()) ReadHeredocs();
    }

    void SkipNewlines() {
        for (;;) {
            SkipBlanksAndComment();
            if (C() == '\n') {
                ConsumeNewline();
            } else {
                break;
            }
        }
    }

    // --- Errors --------------------------------------------------------

    [[noreturn]] void Fail(size_t pos, size_t end, const std::string &message,
                           const std::string &code = "syntax-error") {
        throw ParseFailure{pos, end, code, message};
    }

    std::string TokenTextAt(size_t p) {
        size_t q = p;
        while (q < end_ && !IsMeta(s_[q])) q++;
        return s_.substr(p, q - p);
    }

    // `word_wanted`: a word is required here, and bash, which ends its input
    // with a newline, reports that newline rather than the end of the file.
    [[noreturn]] void Unexpected(bool word_wanted = false) {
        const size_t p = Here();
        if (p >= end_ && word_wanted) Fail(p, p, "syntax error near unexpected token `newline'");
        if (p >= end_) {
            std::string m = "syntax error: unexpected end of file";
            if (!open_.empty()) {
                m += " from `" + open_.back().first + "' command on line " + std::to_string(open_.back().second);
            }
            Fail(p, p, m, "unexpected-eof");
        }
        const std::string op = PeekOp();
        std::string tok;
        if (op == "\n") {
            tok = "newline";
        } else if (!op.empty()) {
            tok = op;
        } else {
            tok = TokenTextAt(p);
            if (tok.empty()) tok = std::string(1, s_[p]);
        }
        const size_t len = op == "\n" ? 1 : tok.size();
        Fail(p, p + len, "syntax error near unexpected token `" + tok + "'");
    }

    // --- Tokens --------------------------------------------------------

    std::string PeekOp() {
        const char c = C(), c1 = C1(), c2 = Ck(2);
        switch (c) {
            case ';':
                if (c1 == ';') return c2 == '&' ? ";;&" : ";;";
                if (c1 == '&') return ";&";
                return ";";
            case '&':
                if (c1 == '&') return "&&";
                if (c1 == '>') return c2 == '>' ? "&>>" : "&>";
                return "&";
            case '|':
                if (c1 == '|') return "||";
                if (c1 == '&') return "|&";
                return "|";
            case '(':
                return "(";
            case ')':
                return ")";
            case '<':
                if (c1 == '<') {
                    if (c2 == '<') return "<<<";
                    if (c2 == '-') return "<<-";
                    return "<<";
                }
                if (c1 == '&') return "<&";
                if (c1 == '>') return "<>";
                if (c1 == '(') return "<(";
                return "<";
            case '>':
                if (c1 == '>') return ">>";
                if (c1 == '&') return ">&";
                if (c1 == '|') return ">|";
                if (c1 == '(') return ">(";
                return ">";
            case '\n':
                return "\n";
            default:
                return "";
        }
    }

    // The plain characters at the cursor, if they form a whole word (a
    // metacharacter or the end follows); "" otherwise.
    std::string PeekPlainWord(size_t *after = nullptr) {
        size_t p = Norm(i_);
        std::string w;
        while (p < end_) {
            const char c = s_[p];
            if (IsMeta(c) || c == '"' || c == '\'' || c == '\\' || c == '$' || c == '`') break;
            w += c;
            p = Norm(p + 1);
        }
        if (!IsMeta(At(p))) return "";
        if (after) *after = p;
        return w;
    }

    bool AtReserved(const char *kw) { return PeekPlainWord() == kw; }

    BashRange ConsumeReserved(BashCommand *cmd) {
        size_t after = 0;
        const size_t start = Here();
        PeekPlainWord(&after);
        i_ = after;
        const BashRange r = Range(start, after);
        if (cmd) cmd->keywords.push_back(r);
        return r;
    }

    void ExpectReserved(const char *kw, BashCommand *cmd) {
        SkipNewlines();
        if (!AtReserved(kw)) Unexpected();
        ConsumeReserved(cmd);
    }

    // --- Here-documents --------------------------------------------------

    void ReadHeredocs() {
        std::vector<PendingHeredoc> pend;
        pend.swap(pending_);
        for (const PendingHeredoc &h : pend) {
            HeredocResult r;
            const size_t body_start = i_;
            size_t body_end = end_;
            size_t after = end_;
            bool found = false;
            size_t p = i_;
            while (p < end_) {
                size_t eol = s_.find('\n', p);
                if (eol == std::string::npos || eol > end_) eol = end_;
                size_t q = p;
                if (h.strip_tabs) {
                    while (q < eol && s_[q] == '\t') q++;
                }
                if (s_.compare(q, eol - q, h.delimiter) == 0 && eol - q == h.delimiter.size()) {
                    body_end = p;
                    after = eol < end_ ? eol + 1 : eol;
                    found = true;
                    break;
                }
                if (eol >= end_) break;
                p = eol + 1;
            }
            r.body = Range(body_start, body_end);
            res_.heredoc_bodies.push_back(Range(body_start, found ? (after > body_start ? after - 1 : after) : end_));
            if (!found) {
                r.unterminated = true;
                if (!res_.has_syntax_error) {
                    BashParseError e;
                    int line = 0, col = 0;
                    LineCol(h.op_pos, &line, &col);
                    e.range = Range(h.op_pos, h.op_pos + 2);
                    e.code = "unterminated-heredoc";
                    e.message = "here-document at line " + std::to_string(line + 1) +
                                " delimited by end-of-file (wanted `" + h.delimiter + "')";
                    e.warning = true;
                    res_.errors.push_back(e);
                }
            }
            if (!h.quoted && body_end > body_start) {
                const size_t save_i = i_, save_end = end_;
                i_ = body_start;
                end_ = body_end;
                try {
                    ReadParts(Mode::Heredoc, &r.parts, false, 0);
                } catch (const ParseFailure &) {
                    // An unclosed expansion in a body: bash reports it when
                    // the here-document is expanded, not when it is read.
                }
                i_ = save_i;
                end_ = save_end;
            }
            heredocs_[h.id] = std::move(r);
            i_ = after;
        }
    }

    void FixHeredocs(BashList &list) {
        for (BashAndOr &ao : list.items) {
            for (BashPipeline &pl : ao.pipelines) {
                for (BashCommand &c : pl.commands) FixHeredocs(c);
            }
        }
    }
    void FixHeredocs(BashCommand &c) {
        for (BashRedirect &r : c.redirects) {
            if (r.heredoc_id >= 0) {
                auto it = heredocs_.find(r.heredoc_id);
                if (it != heredocs_.end()) {
                    r.body_range = it->second.body;
                    r.body_parts = std::move(it->second.parts);
                    r.unterminated = it->second.unterminated;
                } else {
                    r.unterminated = true;  // the line never ended
                }
                for (BashWordPart &p : r.body_parts) FixHeredocs(p);
            }
            FixHeredocs(r.target);
        }
        for (BashAssignment &a : c.assignments) {
            FixHeredocs(a.value);
            for (BashWord &w : a.elements) FixHeredocs(w);
        }
        for (BashWord &w : c.words) FixHeredocs(w);
        for (auto &b : c.bodies) {
            if (b) FixHeredocs(*b);
        }
        for (BashCommand &s : c.sub) FixHeredocs(s);
        for (BashCaseItem &ci : c.case_items) {
            for (BashWord &w : ci.patterns) FixHeredocs(w);
            if (ci.body) FixHeredocs(*ci.body);
        }
        FixHeredocs(c.subject);
        for (BashCondTerm &t : c.cond) FixHeredocs(t.word);
        for (BashWord &w : c.items) FixHeredocs(w);
    }
    void FixHeredocs(BashWord &w) {
        for (BashWordPart &p : w.parts) FixHeredocs(p);
    }
    void FixHeredocs(BashWordPart &p) {
        for (BashWordPart &q : p.parts) FixHeredocs(q);
        for (BashWord &w : p.args) FixHeredocs(w);
        if (p.body) FixHeredocs(*p.body);
    }

    // --- Lists ---------------------------------------------------------

    struct Stop {
        std::vector<const char *> words;
        bool paren = false;      // `)`
        bool case_term = false;  // `;;`, `;&`, `;;&`
        bool brace = false;      // `}`, even with text after it: ${ list; }
    };

    bool AtStop(const Stop &stop) {
        if (stop.paren && C() == ')') return true;
        if (stop.brace && C() == '}') return true;
        if (stop.case_term) {
            const std::string op = PeekOp();
            if (op == ";;" || op == ";&" || op == ";;&") return true;
        }
        if (!stop.words.empty()) {
            const std::string w = PeekPlainWord();
            for (const char *kw : stop.words) {
                if (w == kw) return true;
            }
        }
        return false;
    }

    void ParseTopLevel() {
        for (;;) {
            SkipNewlines();
            if (AtEnd()) return;
            if (recovering_ && SkipStrayCloser()) continue;
            BashAndOr ao = ParseAndOr();
            ParseTerminator(&ao, Stop{});
            res_.program.items.push_back(std::move(ao));
        }
    }

    // After a syntax error, the closing half of whatever construct the error
    // was in is skipped rather than reported again.
    bool SkipStrayCloser() {
        const std::string w = PeekPlainWord();
        if (IsCloser(w) || w == "in") {
            ConsumeReserved(nullptr);
            return true;
        }
        const std::string op = PeekOp();
        if (op == ")" || op == ";;" || op == ";&" || op == ";;&" || op == ";" || op == "&" || op == "|" ||
            op == "||" || op == "&&" || op == "|&") {
            AdvN(op.size());
            return true;
        }
        return false;
    }

    void ParseTerminator(BashAndOr *ao, const Stop &stop) {
        SkipBlanksAndComment();
        const size_t at = Here();
        const std::string op = PeekOp();
        if (op == ";" || op == "&") {
            ao->terminator = op;
            ao->terminator_range = Range(at, at + 1);
            Adv();
        } else if (op == "\n") {
            ao->terminator = "\n";
            ao->terminator_range = Range(at, at + 1);
            ConsumeNewline();
        } else if (AtEnd() || AtStop(stop)) {
            // The list ends here.
        } else {
            Unexpected();
        }
    }

    std::shared_ptr<BashList> ParseList(const Stop &stop, bool allow_empty) {
        DepthGuard guard(this);
        auto list = std::make_shared<BashList>();
        SkipNewlines();
        const size_t start = Here();
        for (;;) {
            SkipNewlines();
            if (AtEnd() || AtStop(stop)) break;
            BashAndOr ao = ParseAndOr();
            ParseTerminator(&ao, stop);
            list->items.push_back(std::move(ao));
        }
        if (!allow_empty && list->items.empty()) Unexpected();
        list->range = Range(start, list->items.empty() ? start : RangeEndOf(list->items.back()));
        return list;
    }

    size_t RangeEndOf(const BashAndOr &ao) { return OffsetOf(ao.range.end_line, ao.range.end_col); }
    size_t OffsetOf(int line, int col) const { return line_starts_[static_cast<size_t>(line)] + static_cast<size_t>(col); }

    BashAndOr ParseAndOr() {
        BashAndOr ao;
        const size_t start = Here();
        ao.pipelines.push_back(ParsePipeline());
        for (;;) {
            SkipBlanks();
            const std::string op = PeekOp();
            if (op != "&&" && op != "||") break;
            AdvN(2);
            ao.ops.push_back(op);
            SkipNewlines();
            ao.pipelines.push_back(ParsePipeline());
        }
        ao.range = Range(start, last_end_);
        return ao;
    }

    size_t last_end_ = 0;  // the end of the last command parsed

    BashPipeline ParsePipeline() {
        BashPipeline pl;
        SkipBlanks();
        const size_t start = Here();
        bool prefixed = false;
        for (;;) {
            if (AtReserved("time")) {
                ConsumeReserved(nullptr);
                pl.timed = true;
                SkipBlanks();
                while (PeekPlainWord() == "-p" || PeekPlainWord() == "--") {
                    ConsumeReserved(nullptr);
                    SkipBlanks();
                }
            } else if (AtReserved("!")) {
                ConsumeReserved(nullptr);
                pl.negated = !pl.negated;
                SkipBlanks();
            } else {
                break;
            }
            prefixed = true;
            last_end_ = Here();
        }
        if (prefixed) {
            // `time` alone times nothing; `!` alone negates nothing.
            const std::string op = PeekOp();
            if (AtEnd() || op == ";" || op == "\n" || op == "&" || op == ")" || op == "&&" || op == "||") {
                pl.range = Range(start, last_end_);
                return pl;
            }
        }
        pl.commands.push_back(ParseCommand());
        for (;;) {
            SkipBlanks();
            const std::string op = PeekOp();
            if (op != "|" && op != "|&") break;
            AdvN(op.size());
            pl.ops.push_back(op);
            SkipNewlines();
            pl.commands.push_back(ParseCommand());
        }
        pl.range = Range(start, last_end_);
        return pl;
    }

    // --- Commands --------------------------------------------------------

    BashCommand ParseCommand() {
        DepthGuard guard(this);
        SkipBlanks();
        const size_t start = Here();
        BashCommand cmd;
        const char c = C();
        bool compound = true;
        if (c == '(') {
            if (C1() == '(' && LooksLikeArith(Here() + 0, 2)) {
                ParseArithCommand(&cmd);
            } else {
                ParseSubshell(&cmd);
            }
        } else {
            const std::string w = PeekPlainWord();
            if (w == "{") {
                ParseGroup(&cmd);
            } else if (w == "if") {
                ParseIf(&cmd);
            } else if (w == "while" || w == "until") {
                ParseWhile(&cmd, w == "while");
            } else if (w == "for") {
                ParseFor(&cmd, false);
            } else if (w == "select") {
                ParseFor(&cmd, true);
            } else if (w == "case") {
                ParseCase(&cmd);
            } else if (w == "function") {
                ParseFunctionKeyword(&cmd);
            } else if (w == "[[") {
                ParseCond(&cmd);
            } else if (w == "coproc") {
                ParseCoproc(&cmd);
            } else if (IsCloser(w)) {
                Unexpected();
            } else {
                compound = false;
                ParseSimple(&cmd);
            }
        }
        if (compound) ParseRedirects(&cmd);
        cmd.range = Range(start, last_end_);
        return cmd;
    }

    void ParseRedirects(BashCommand *cmd) {
        for (;;) {
            SkipBlanks();
            if (!AtRedirect()) break;
            cmd->redirects.push_back(ParseRedirect());
        }
        if (!cmd->redirects.empty()) last_end_ = OffsetOf(cmd->redirects.back().range.end_line,
                                                          cmd->redirects.back().range.end_col);
    }

    void ParseSubshell(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Subshell;
        const size_t a = Here();
        OpenGuard open(this, "(", a);
        Adv();
        cmd->keywords.push_back(Range(a, a + 1));
        Stop stop;
        stop.paren = true;
        cmd->bodies.push_back(ParseList(stop, false));
        SkipNewlines();
        if (C() != ')') Unexpected();
        const size_t b = Here();
        Adv();
        cmd->keywords.push_back(Range(b, b + 1));
        last_end_ = b + 1;
    }

    void ParseGroup(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Group;
        OpenGuard open(this, "{", Here());
        ConsumeReserved(cmd);
        Stop stop;
        stop.words = {"}"};
        cmd->bodies.push_back(ParseList(stop, false));
        ExpectReserved("}", cmd);
        last_end_ = Here();
    }

    void ParseIf(BashCommand *cmd) {
        cmd->kind = BashCommandKind::If;
        OpenGuard open(this, "if", Here());
        ConsumeReserved(cmd);
        Stop then_stop;
        then_stop.words = {"then"};
        Stop body_stop;
        body_stop.words = {"elif", "else", "fi"};
        cmd->bodies.push_back(ParseList(then_stop, false));
        ExpectReserved("then", cmd);
        cmd->bodies.push_back(ParseList(body_stop, false));
        for (;;) {
            SkipNewlines();
            if (AtReserved("elif")) {
                ConsumeReserved(cmd);
                cmd->bodies.push_back(ParseList(then_stop, false));
                ExpectReserved("then", cmd);
                cmd->bodies.push_back(ParseList(body_stop, false));
            } else if (AtReserved("else")) {
                ConsumeReserved(cmd);
                Stop fi_stop;
                fi_stop.words = {"fi"};
                cmd->bodies.push_back(ParseList(fi_stop, false));
                cmd->has_else = true;
                break;
            } else {
                break;
            }
        }
        ExpectReserved("fi", cmd);
        last_end_ = Here();
    }

    void ParseWhile(BashCommand *cmd, bool is_while) {
        cmd->kind = is_while ? BashCommandKind::While : BashCommandKind::Until;
        OpenGuard open(this, is_while ? "while" : "until", Here());
        ConsumeReserved(cmd);
        Stop do_stop;
        do_stop.words = {"do"};
        cmd->bodies.push_back(ParseList(do_stop, false));
        ParseDoGroup(cmd, false);
    }

    // `do list done`, or for a for/select loop also `{ list }`.
    void ParseDoGroup(BashCommand *cmd, bool brace_ok) {
        SkipNewlines();
        if (brace_ok && AtReserved("{")) {
            ConsumeReserved(cmd);
            Stop stop;
            stop.words = {"}"};
            cmd->bodies.push_back(ParseList(stop, false));
            ExpectReserved("}", cmd);
        } else {
            ExpectReserved("do", cmd);
            Stop stop;
            stop.words = {"done"};
            cmd->bodies.push_back(ParseList(stop, false));
            ExpectReserved("done", cmd);
        }
        last_end_ = Here();
    }

    void ParseFor(BashCommand *cmd, bool select) {
        cmd->kind = select ? BashCommandKind::Select : BashCommandKind::For;
        OpenGuard open(this, select ? "select" : "for", Here());
        ConsumeReserved(cmd);
        SkipBlanks();
        if (!select && C() == '(' && C1() == '(') {
            cmd->kind = BashCommandKind::ForArith;
            const size_t a = Here();
            AdvN(2);
            cmd->keywords.push_back(Range(a, a + 2));
            for (int k = 0; k < 3; k++) {
                const char *close = k < 2 ? ";" : "))";
                cmd->ariths.push_back(ReadArith(close));
                if (k < 2) {
                    if (C() != ';') Unexpected();
                    Adv();
                }
            }
            AdvN(2);
            SkipBlanks();
            if (C() == ';') Adv();
            ParseDoGroup(cmd, true);
            return;
        }
        if (IsMeta(C())) Unexpected(true);
        const BashWord name = ParseWord();
        cmd->var = name.text;
        cmd->var_range = name.range;
        // `for 1 in ...` is an error when it runs (not a valid identifier),
        // not when it is read: the linter reports it.
        SkipNewlines();
        if (AtReserved("in")) {
            ConsumeReserved(cmd);
            cmd->has_in = true;
            for (;;) {
                SkipBlanks();
                const char c = C();
                if (c == ';' || c == '\n' || c == 0) break;
                if (c == '#') {
                    SkipComment();
                    continue;
                }
                if (IsMeta(c)) Unexpected();
                cmd->items.push_back(ParseWord());
            }
            if (C() == ';' && C1() != ';') {
                Adv();
            } else if (C() == '\n') {
                ConsumeNewline();
            } else {
                Unexpected();
            }
        } else if (C() == ';') {
            Adv();
        }
        ParseDoGroup(cmd, true);
    }

    void ParseCase(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Case;
        OpenGuard open(this, "case", Here());
        ConsumeReserved(cmd);
        SkipBlanks();
        if (IsMeta(C())) Unexpected(true);
        cmd->subject = ParseWord();
        SkipNewlines();
        if (!AtReserved("in")) Unexpected();
        ConsumeReserved(cmd);
        for (;;) {
            SkipNewlines();
            if (AtReserved("esac")) {
                ConsumeReserved(cmd);
                break;
            }
            BashCaseItem item;
            const size_t item_start = Here();
            if (C() == '(') {
                Adv();
                SkipBlanks();
            }
            for (;;) {
                SkipBlanks();
                if (IsMeta(C())) Unexpected();
                item.patterns.push_back(ParseWord());
                SkipBlanks();
                if (C() == '|') {
                    Adv();
                    continue;
                }
                if (C() == ')') {
                    Adv();
                    break;
                }
                Unexpected();
            }
            Stop stop;
            stop.words = {"esac"};
            stop.case_term = true;
            item.body = ParseList(stop, true);
            SkipNewlines();
            const std::string op = PeekOp();
            size_t item_end = Here();
            if (op == ";;" || op == ";&" || op == ";;&") {
                item.terminator = op;
                AdvN(op.size());
                item_end = Here();
                item.range = Range(item_start, item_end);
                cmd->case_items.push_back(std::move(item));
                continue;
            }
            item.range = Range(item_start, item_end);
            cmd->case_items.push_back(std::move(item));
            if (!AtReserved("esac")) Unexpected();
            ConsumeReserved(cmd);
            break;
        }
        last_end_ = Here();
    }

    void ParseFunctionKeyword(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Function;
        cmd->function_keyword = true;
        ConsumeReserved(cmd);
        SkipBlanks();
        if (IsMeta(C())) Unexpected(true);
        const BashWord name = ParseWord();
        cmd->name = name.text;
        cmd->name_range = name.range;
        SkipBlanks();
        if (C() == '(') {
            Adv();
            SkipBlanks();
            if (C() != ')') Unexpected();
            Adv();
        }
        ParseFunctionBody(cmd);
    }

    void ParseFunctionBody(BashCommand *cmd) {
        SkipNewlines();
        const char c = C();
        const std::string w = PeekPlainWord();
        const bool compound = c == '(' || w == "{" || w == "if" || w == "while" || w == "until" || w == "for" ||
                              w == "select" || w == "case" || w == "[[";
        if (!compound) Unexpected();
        cmd->sub.push_back(ParseCommand());
        last_end_ = OffsetOf(cmd->sub.back().range.end_line, cmd->sub.back().range.end_col);
    }

    void ParseCoproc(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Coproc;
        ConsumeReserved(cmd);
        SkipBlanks();
        // coproc NAME compound-command: a name followed by a compound command.
        const size_t save = i_;
        size_t after = 0;
        const std::string w = PeekPlainWord(&after);
        if (!w.empty() && !BashIsReservedWord(w)) {
            i_ = after;
            SkipBlanks();
            const std::string next = PeekPlainWord();
            if (C() == '(' || next == "{" || next == "if" || next == "while" || next == "until" || next == "for" ||
                next == "select" || next == "case" || next == "[[") {
                cmd->name = w;
                cmd->name_range = Range(Norm(save), after);
            } else {
                i_ = save;
            }
        }
        cmd->sub.push_back(ParseCommand());
        last_end_ = OffsetOf(cmd->sub.back().range.end_line, cmd->sub.back().range.end_col);
    }

    // Does the `((` at `pos` (with `skip` characters of opening) close with
    // `))`, making it arithmetic, rather than being two subshells?
    bool LooksLikeArith(size_t pos, int skip) {
        size_t p = Norm(pos);
        for (int k = 0; k < skip; k++) p = Norm(p + 1);
        int depth = 0;
        while (p < end_) {
            const char c = s_[p];
            if (c == '\\') {
                p = Norm(p + 2);
                continue;
            }
            if (c == '\'' || c == '"') {
                const size_t q = s_.find(c, p + 1);
                if (q == std::string::npos) return false;
                p = Norm(q + 1);
                continue;
            }
            if (c == '(') depth++;
            if (c == ')') {
                if (depth == 0) return At(Norm(p + 1)) == ')';
                depth--;
            }
            p = Norm(p + 1);
        }
        return true;  // unclosed: bash reads it as arithmetic and reports the missing `))'
    }

    void ParseArithCommand(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Arith;
        const size_t a = Here();
        AdvN(2);
        cmd->keywords.push_back(Range(a, a + 2));
        cmd->ariths.push_back(ReadArith("))"));
        const size_t b = Here();
        AdvN(2);
        cmd->keywords.push_back(Range(b, b + 2));
        last_end_ = Here();
    }

    // --- [[ ]] -------------------------------------------------------------

    static bool IsUnaryTest(const std::string &w) {
        static const char *const ops[] = {"-a", "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k", "-p", "-r",
                                          "-s", "-t", "-u", "-w", "-x", "-G", "-L", "-N", "-O", "-S", "-z",
                                          "-n", "-o", "-v", "-R"};
        for (const char *op : ops) {
            if (w == op) return true;
        }
        return false;
    }
    static bool IsBinaryTest(const std::string &w) {
        static const char *const ops[] = {"==", "=",   "!=",  "=~",  "<",   ">",   "-eq", "-ne",
                                          "-lt", "-le", "-gt", "-ge", "-nt", "-ot", "-ef"};
        for (const char *op : ops) {
            if (w == op) return true;
        }
        return false;
    }

    // Inside [[ ]] newlines are skipped between terms, except right after
    // a lone word: there bash wants a binary operator, and a newline is an
    // error (`[[ a` newline `]]`), while `[[ -n a` newline `&& b ]]` is fine.
    void SkipCondBlanks() {
        for (;;) {
            SkipBlanks();
            if (C() == '\n') {
                ConsumeNewline();
            } else {
                break;
            }
        }
    }

    bool AtCondEnd() {
        SkipBlanks();
        const size_t p = Here();
        return At(p) == ']' && At(Norm(p + 1)) == ']' && IsMeta(At(Norm(Norm(p + 1) + 1)));
    }

    bool AtCondNewline() {
        SkipBlanks();
        return C() == '\n' || AtEnd();
    }

    void ParseCond(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Cond;
        ConsumeReserved(cmd);
        SkipCondBlanks();
        if (AtCondEnd()) Fail(Here(), Here() + 2, "syntax error near `]]'");
        ParseCondOr(cmd);
        SkipCondBlanks();
        if (!AtCondEnd()) {
            const size_t p = Here();
            Fail(p, p + std::max<size_t>(1, TokenTextAt(p).size()),
                 "syntax error in conditional expression: unexpected token `" + CondTokenText() + "'");
        }
        const size_t b = Here();
        AdvN(2);
        cmd->keywords.push_back(Range(b, b + 2));
        last_end_ = Here();
    }

    std::string CondTokenText() {
        if (AtEnd()) return "newline";
        const std::string op = PeekOp();
        if (!op.empty()) return op == "\n" ? "newline" : op;
        return TokenTextAt(Here());
    }

    void PushCondOp(BashCommand *cmd, BashCondTerm::Kind kind, size_t len) {
        BashCondTerm t;
        t.kind = kind;
        const size_t a = Here();
        AdvN(len);
        t.word.range = Range(a, Here());
        t.word.text = s_.substr(a, Here() - a);
        cmd->cond.push_back(std::move(t));
    }

    void ParseCondOr(BashCommand *cmd) {
        DepthGuard guard(this);
        ParseCondAnd(cmd);
        for (;;) {
            SkipCondBlanks();
            if (C() == '|' && C1() == '|') {
                PushCondOp(cmd, BashCondTerm::Or, 2);
                ParseCondAnd(cmd);
            } else {
                break;
            }
        }
    }

    void ParseCondAnd(BashCommand *cmd) {
        ParseCondTerm(cmd);
        for (;;) {
            SkipCondBlanks();
            if (C() == '&' && C1() == '&') {
                PushCondOp(cmd, BashCondTerm::And, 2);
                ParseCondTerm(cmd);
            } else {
                break;
            }
        }
    }

    [[noreturn]] void CondFail(const std::string &message) {
        const size_t p = Here();
        Fail(p, p + (AtEnd() ? 0 : std::max<size_t>(1, TokenTextAt(p).size())), message);
    }

    void ParseCondTerm(BashCommand *cmd) {
        DepthGuard guard(this);
        SkipCondBlanks();
        if (AtEnd()) Fail(Here(), Here(), "unexpected EOF while looking for `]]'", "unexpected-eof");
        if (AtCondEnd()) CondFail("syntax error in conditional expression");
        if (C() == '!' && IsMeta(C1())) {
            PushCondOp(cmd, BashCondTerm::Not, 1);
            ParseCondTerm(cmd);
            return;
        }
        if (C() == '(') {
            PushCondOp(cmd, BashCondTerm::Open, 1);
            ParseCondOr(cmd);
            SkipCondBlanks();
            if (C() != ')') CondFail("syntax error in conditional expression: expected `)'");
            PushCondOp(cmd, BashCondTerm::Close, 1);
            return;
        }
        BashCondTerm first;
        first.word = ParseCondWord();
        const std::string w1 = first.word.text;
        if (IsUnaryTest(w1) && !AtCondEnd()) {
            if (AtCondNewline()) CondFail("unexpected argument `newline' to conditional unary operator");
            if (!(C() == '&' && C1() == '&') && !(C() == '|' && C1() == '|') && C() != ')') {
                const size_t save = i_;
                BashWord operand = ParseCondWord();
                SkipBlanks();
                // `-a` and friends can also be a plain string compared by a
                // binary operator: [[ -n == x ]].
                if (!IsBinaryTest(PeekCondWordText())) {
                    first.kind = BashCondTerm::UnaryOp;
                    cmd->cond.push_back(std::move(first));
                    BashCondTerm t;
                    t.word = std::move(operand);
                    cmd->cond.push_back(std::move(t));
                    return;
                }
                i_ = save;
            }
        }
        cmd->cond.push_back(first);
        if (AtCondEnd()) return;
        SkipBlanks();
        if ((C() == '&' && C1() == '&') || (C() == '|' && C1() == '|') || C() == ')') return;
        const std::string op = AtCondNewline() ? "newline" : PeekCondWordText();
        if (!IsBinaryTest(op)) {
            CondFail("unexpected token `" + (op.empty() ? CondTokenText() : op) +
                     "', conditional binary operator expected");
        }
        BashCondTerm bop;
        bop.kind = BashCondTerm::BinaryOp;
        bop.word = ParseCondWord();
        cmd->cond.push_back(bop);
        SkipBlanks();
        if (AtCondNewline()) CondFail("unexpected argument `newline' to conditional binary operator");
        if (AtCondEnd()) CondFail("unexpected argument `]]' to conditional binary operator");
        BashCondTerm rhs;
        if (op == "=~") {
            rhs.word = ParseRegexWord();
            rhs.pattern = true;
        } else {
            rhs.word = ParseCondWord();
            rhs.pattern = op == "==" || op == "=" || op == "!=";
        }
        cmd->cond.push_back(std::move(rhs));
    }

    std::string PeekCondWordText() {
        const char c = C();
        if (c == '<' || c == '>') return std::string(1, c);
        return TokenTextAt(Here());
    }

    // A word inside [[ ]]: `<` and `>` are words (string comparisons), and
    // `&&`, `||`, `(`, `)` end it.
    BashWord ParseCondWord() {
        const size_t start = Here();
        BashWord w;
        const char c = C();
        if (c == '<' || c == '>') {
            Adv();
        } else {
            if (IsMeta(c)) {
                if (AtEnd()) Fail(Here(), Here(), "unexpected EOF while looking for `]]'", "unexpected-eof");
                Fail(Here(), Here() + 1, "syntax error in conditional expression: unexpected token `" +
                                             CondTokenText() + "'");
            }
            ReadParts(Mode::Normal, &w.parts, false, 0);
        }
        w.range = Range(start, Here());
        w.text = s_.substr(start, Here() - start);
        last_end_ = Here();
        return w;
    }

    // The right side of =~: a regex, which may hold parentheses and `|` and
    // ends at a blank outside them.
    BashWord ParseRegexWord() {
        const size_t start = Here();
        BashWord w;
        ReadParts(Mode::Regex, &w.parts, false, 0);
        w.range = Range(start, Here());
        w.text = s_.substr(start, Here() - start);
        if (w.text.empty()) Unexpected();
        return w;
    }

    // --- Simple commands ---------------------------------------------------

    bool AtRedirect() {
        const char c = C();
        if (c == '<' || c == '>') return C1() != '(';
        if (c == '&' && C1() == '>') return true;
        size_t p = Here();
        if (IsDigit(At(p))) {
            while (IsDigit(At(p))) p = Norm(p + 1);
            const char d = At(p);
            return (d == '<' || d == '>') && At(Norm(p + 1)) != '(';
        }
        if (c == '{') {
            p = Norm(p + 1);
            if (!IsNameStart(At(p))) return false;
            while (IsNameChar(At(p))) p = Norm(p + 1);
            if (At(p) != '}') return false;
            p = Norm(p + 1);
            return At(p) == '<' || At(p) == '>';
        }
        return false;
    }

    BashRedirect ParseRedirect() {
        BashRedirect r;
        const size_t start = Here();
        if (IsDigit(C())) {
            const size_t a = Here();
            while (IsDigit(C())) {
                r.fd += C();
                Adv();
            }
            r.fd_range = Range(a, Here());
        } else if (C() == '{') {
            const size_t a = Here();
            while (C() != '}') {
                r.fd += C();
                Adv();
            }
            r.fd += '}';
            Adv();
            r.fd_range = Range(a, Here());
        }
        r.op = PeekOp();
        if (r.op == "<(" || r.op == ">(") r.op = r.op.substr(0, 1);
        AdvN(r.op.size());
        SkipBlanks();
        if (IsMeta(C()) && !((C() == '<' || C() == '>') && C1() == '(')) Unexpected(true);
        r.target = ParseWord();
        if (r.op == "<<" || r.op == "<<-") {
            r.heredoc = true;
            r.heredoc_quoted = r.target.text.find_first_of("'\"\\") != std::string::npos;
            r.delimiter = HeredocDelimiter(r.target.text);
            r.heredoc_id = next_heredoc_++;
            pending_.push_back(PendingHeredoc{r.heredoc_id, r.delimiter, r.heredoc_quoted, r.op == "<<-", start});
        }
        r.range = Range(start, Here());
        last_end_ = Here();
        return r;
    }

    // Quote removal on a here-document delimiter word.
    static std::string HeredocDelimiter(const std::string &raw) {
        std::string out;
        char quote = 0;
        for (size_t k = 0; k < raw.size(); k++) {
            const char c = raw[k];
            if (quote) {
                if (c == quote) {
                    quote = 0;
                } else if (quote == '"' && c == '\\' && k + 1 < raw.size() &&
                           std::strchr("$`\"\\", raw[k + 1]) != nullptr) {
                    out += raw[++k];
                } else {
                    out += c;
                }
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '\\' && k + 1 < raw.size()) {
                if (raw[k + 1] == '\n') {
                    k++;
                } else {
                    out += raw[++k];
                }
            } else if (c == '$' && k + 1 < raw.size() && (raw[k + 1] == '\'' || raw[k + 1] == '"')) {
                // $'...' / $"..." delimiters: the quotes are what matter.
            } else {
                out += c;
            }
        }
        return out;
    }

    // name=, name+=, name[...]= at the cursor? Sets *len to the length of the
    // prefix including the `=`.
    bool LooksLikeAssignment() {
        size_t p = Here();
        if (!IsNameStart(At(p))) return false;
        while (IsNameChar(At(p))) p = Norm(p + 1);
        if (At(p) == '[') {
            int depth = 0;
            while (p < end_) {
                const char c = At(p);
                if (c == '\\') {
                    p = Norm(p + 2);
                    continue;
                }
                if (c == '\'' || c == '"') {
                    const size_t q = s_.find(c, p + 1);
                    if (q == std::string::npos || q >= end_) return false;
                    p = Norm(q + 1);
                    continue;
                }
                if (c == '\n') return false;
                if (c == '[') depth++;
                if (c == ']' && --depth == 0) break;
                p = Norm(p + 1);
            }
            if (At(p) != ']') return false;
            p = Norm(p + 1);
        }
        if (At(p) == '+') p = Norm(p + 1);
        return At(p) == '=';
    }

    // At an assignment (LooksLikeAssignment), is its value an array: `=(`?
    bool ArrayAssignmentAhead() {
        size_t p = Here();
        while (p < end_ && At(p) != '=') {
            if (At(p) == '\'' || At(p) == '"') {
                const size_t q = s_.find(At(p), p + 1);
                if (q == std::string::npos) return false;
                p = q;
            }
            p = Norm(p + 1);
        }
        return At(p) == '=' && At(Norm(p + 1)) == '(';
    }

    BashAssignment ParseAssignment() {
        BashAssignment a;
        const size_t start = Here();
        while (IsNameChar(C())) {
            a.name += C();
            Adv();
        }
        a.name_range = Range(start, Here());
        if (C() == '[') {
            Adv();
            a.has_subscript = true;
            const size_t sa = Here();
            a.subscript_arith = ReadArith("]");
            a.subscript = s_.substr(sa, Here() - sa);
            Adv();  // ]
        }
        if (C() == '+') {
            a.append = true;
            Adv();
        }
        Adv();  // =
        const size_t value_start = Here();
        if (C() == '(') {
            a.is_array = true;
            Adv();
            for (;;) {
                SkipNewlines();
                if (C() == ')') {
                    Adv();
                    break;
                }
                if (AtEnd()) Fail(Here(), Here(), "unexpected EOF while looking for matching `)'", "unexpected-eof");
                if (IsMeta(C())) Unexpected();
                a.elements.push_back(ParseWord());
            }
            a.value.range = Range(value_start, Here());
            a.value.text = s_.substr(value_start, Here() - value_start);
        } else if (!IsMeta(C())) {
            a.value = ParseWord();
        } else {
            a.value.range = Range(value_start, value_start);
        }
        a.range = Range(start, Here());
        last_end_ = Here();
        return a;
    }

    void ParseSimple(BashCommand *cmd) {
        cmd->kind = BashCommandKind::Simple;
        bool declaration = false;
        bool takes_assignments = false;
        for (;;) {
            SkipBlanks();
            const char c = C();
            if (c == '#') {
                // A `#` starting a word starts a comment, which ends the
                // command; the newline after it is the terminator.
                SkipComment();
                break;
            }
            if (AtRedirect()) {
                cmd->redirects.push_back(ParseRedirect());
                continue;
            }
            const bool procsub = (c == '<' || c == '>') && C1() == '(';
            if (IsMeta(c) && !procsub) break;
            // name=(...) is an assignment before the command name and an
            // argument of declare & co.; bash also reads it, as one word, after
            // alias, eval and let (`eval a=( $x )`), and nowhere else.
            if (!cmd->words.empty() && !declaration && takes_assignments && LooksLikeAssignment() &&
                ArrayAssignmentAhead()) {
                BashAssignment a = ParseAssignment();
                BashWord w;
                w.range = a.range;
                const size_t s0 = OffsetOf(a.range.line, a.range.col);
                w.text = s_.substr(s0, OffsetOf(a.range.end_line, a.range.end_col) - s0);
                for (BashWord &e : a.elements) {
                    for (BashWordPart &part : e.parts) w.parts.push_back(std::move(part));
                }
                cmd->words.push_back(std::move(w));
                continue;
            }
            if ((cmd->words.empty() || declaration) && LooksLikeAssignment()) {
                BashAssignment a = ParseAssignment();
                if (declaration) {
                    a.declaration = true;
                    BashWord w;
                    w.range = a.range;
                    const size_t s0 = OffsetOf(a.range.line, a.range.col);
                    w.text = s_.substr(s0, OffsetOf(a.range.end_line, a.range.end_col) - s0);
                    w.parts = a.value.parts;
                    cmd->words.push_back(std::move(w));
                }
                cmd->assignments.push_back(std::move(a));
                continue;
            }
            BashWord w = ParseWord();
            if (cmd->words.empty()) {
                // name () compound-command: a function definition.
                const size_t save = i_;
                SkipBlanks();
                if (C() == '(' && cmd->assignments.empty() && cmd->redirects.empty()) {
                    Adv();
                    SkipBlanks();
                    if (C() != ')') Unexpected();
                    Adv();
                    cmd->kind = BashCommandKind::Function;
                    cmd->name = w.text;
                    cmd->name_range = w.range;
                    ParseFunctionBody(cmd);
                    return;
                }
                i_ = save;
                std::string lit;
                if (BashWordLiteral(w, &lit)) {
                    declaration = IsDeclarationBuiltin(lit);
                    takes_assignments = lit == "alias" || lit == "eval" || lit == "let";
                }
            }
            cmd->words.push_back(std::move(w));
        }
        if (cmd->words.empty() && cmd->assignments.empty() && cmd->redirects.empty()) Unexpected();
    }

    // --- Words ---------------------------------------------------------------

    enum class Mode {
        Normal,    // ends at a metacharacter
        DQuote,    // ends at `"`
        Heredoc,   // runs to end_; only $ ` \ are special
        ParamArg,  // ends at `}` (or the extra stop character)
        Regex,     // ends at a blank outside parentheses
        ExtGlob,   // ends at the `)` closing the group
        Subscript, // ends at the `]` closing the subscript
    };

    BashWord ParseWord() {
        const size_t start = Here();
        BashWord w;
        ReadParts(Mode::Normal, &w.parts, false, 0);
        w.range = Range(start, Here());
        w.text = s_.substr(start, Here() - start);
        last_end_ = Here();
        return w;
    }

    void FlushLiteral(std::vector<BashWordPart> *parts, size_t *lit_start, size_t now, bool in_dq) {
        if (*lit_start == std::string::npos) return;
        if (now > *lit_start) {
            BashWordPart p;
            p.kind = BashPartKind::Literal;
            p.range = Range(*lit_start, now);
            p.text = s_.substr(*lit_start, now - *lit_start);
            p.quoted = in_dq;
            parts->push_back(std::move(p));
        }
        *lit_start = std::string::npos;
    }

    // Reads word parts until the mode's end. `extra_stop` is a further
    // character that ends a ParamArg word (`/` or `:`), 0 for none.
    void ReadParts(Mode mode, std::vector<BashWordPart> *parts, bool in_dq, char extra_stop) {
        DepthGuard guard(this);
        size_t lit = std::string::npos;
        int depth = 0;  // parens (Regex, ExtGlob), braces (ParamArg), brackets (Subscript)
        for (;;) {
            const size_t p = Here();
            const char c = At(p);
            if (p >= end_) break;
            bool stop = false;
            switch (mode) {
                case Mode::Normal:
                    stop = IsMeta(c) && !((c == '<' || c == '>') && parts->empty() && lit == std::string::npos &&
                                          C1() == '(');
                    break;
                case Mode::DQuote:
                    stop = c == '"';
                    break;
                case Mode::Heredoc:
                    break;
                case Mode::ParamArg:
                    // The first unquoted `}` closes: bash does not pair braces
                    // here (`${y:-{a}b}` is `{a` then `b}`).
                    stop = c == '}' || (extra_stop && c == extra_stop);
                    break;
                case Mode::Regex:
                    if (c == '(') depth++;
                    if (c == ')') {
                        if (depth == 0) stop = true;
                        depth--;
                    }
                    if ((IsBlank(c) || c == '\n') && depth <= 0) stop = true;
                    break;
                case Mode::ExtGlob:
                    if (c == '(') depth++;
                    if (c == ')') {
                        if (depth == 0) stop = true;
                        depth--;
                    }
                    break;
                case Mode::Subscript:
                    if (c == '[') depth++;
                    if (c == ']') {
                        if (depth == 0) stop = true;
                        depth--;
                    }
                    break;
            }
            if (stop) break;

            if (c == '\\') {
                if (lit == std::string::npos) lit = p;
                const char next = At(p + 1);
                if (mode == Mode::DQuote || mode == Mode::Heredoc) {
                    // Only these are escapes in double quotes and bodies.
                    i_ = (next != 0 && std::strchr("$`\"\\\n", next) != nullptr) ? Norm(p + 2) : Norm(p + 1);
                } else {
                    i_ = next != 0 ? Norm(p + 2) : Norm(p + 1);
                }
                continue;
            }
            // In "${x-'a}b'}" the single quotes are kept as characters, but
            // bash still pairs them when it looks for the closing brace.
            const bool single_ok = mode != Mode::DQuote && mode != Mode::Heredoc;
            if (c == '\'' && single_ok) {
                FlushLiteral(parts, &lit, p, in_dq);
                parts->push_back(ReadSingleQuoted());
                continue;
            }
            if (c == '"' && mode != Mode::DQuote && mode != Mode::Heredoc) {
                FlushLiteral(parts, &lit, p, in_dq);
                parts->push_back(ReadDoubleQuoted(BashPartKind::DoubleQuoted, 1));
                continue;
            }
            if (c == '$') {
                const char n1 = C1();
                const bool expands = n1 == '(' || n1 == '{' || n1 == '[' || IsNameStart(n1) || IsDigit(n1) ||
                                     (n1 != 0 && std::strchr("@*#?-$!", n1) != nullptr) ||
                                     ((n1 == '\'' || n1 == '"') &&
                                      ((!in_dq && mode != Mode::DQuote && mode != Mode::Heredoc) ||
                                       mode == Mode::ParamArg));
                if (expands) {
                    FlushLiteral(parts, &lit, p, in_dq);
                    parts->push_back(ReadDollar(in_dq || mode == Mode::DQuote || mode == Mode::Heredoc));
                    continue;
                }
            }
            if (c == '`') {
                FlushLiteral(parts, &lit, p, in_dq);
                parts->push_back(ReadBackquote(in_dq || mode == Mode::DQuote || mode == Mode::Heredoc));
                continue;
            }
            if ((c == '<' || c == '>') && mode == Mode::Normal && C1() == '(') {
                FlushLiteral(parts, &lit, p, in_dq);
                parts->push_back(ReadProcessSub());
                continue;
            }
            if ((mode == Mode::Normal || mode == Mode::ParamArg || mode == Mode::ExtGlob || mode == Mode::Regex) &&
                std::strchr("?*+@!", c) != nullptr && C1() == '(') {
                FlushLiteral(parts, &lit, p, in_dq);
                parts->push_back(ReadExtGlob());
                continue;
            }
            if (lit == std::string::npos) lit = p;
            Adv();
        }
        FlushLiteral(parts, &lit, Here(), in_dq);
    }

    BashWordPart ReadSingleQuoted() {
        BashWordPart part;
        part.kind = BashPartKind::SingleQuoted;
        const size_t a = Here();
        const size_t q = s_.find('\'', a + 1);
        if (q == std::string::npos || q >= end_) {
            Fail(a, a + 1, "unexpected EOF while looking for matching `''", "unexpected-eof");
        }
        i_ = Norm(q + 1);
        part.range = Range(a, q + 1);
        part.text = s_.substr(a, q + 1 - a);
        return part;
    }

    BashWordPart ReadDoubleQuoted(BashPartKind kind, size_t open_len) {
        BashWordPart part;
        part.kind = kind;
        const size_t a = Here();
        AdvN(open_len);
        ReadParts(Mode::DQuote, &part.parts, true, 0);
        if (C() != '"') Fail(a, a + open_len, "unexpected EOF while looking for matching `\"'", "unexpected-eof");
        Adv();
        part.range = Range(a, Here());
        part.text = s_.substr(a, Here() - a);
        for (BashWordPart &p : part.parts) p.quoted = true;
        return part;
    }

    BashWordPart ReadDollar(bool in_dq) {
        const size_t a = Here();
        const char n1 = C1();
        if (n1 == '\'') {
            // $'...': a backslash escapes the next character, the quote included.
            BashWordPart part;
            part.kind = BashPartKind::AnsiC;
            size_t p = Norm(a + 1) + 1;
            while (p < end_ && s_[p] != '\'') {
                if (s_[p] == '\\') p++;
                p++;
            }
            if (p >= end_) Fail(a, a + 2, "unexpected EOF while looking for matching `''", "unexpected-eof");
            i_ = Norm(p + 1);
            part.range = Range(a, p + 1);
            part.text = s_.substr(a, p + 1 - a);
            part.quoted = in_dq;
            return part;
        }
        if (n1 == '"') {
            BashWordPart part = ReadDoubleQuoted(BashPartKind::Locale, 2);
            return part;
        }
        if (n1 == '(') {
            if (Ck(2) == '(' && LooksLikeArith(a, 3)) {
                BashWordPart part;
                part.kind = BashPartKind::ArithSub;
                AdvN(3);
                part.arith = ReadArith("))");
                AdvN(2);
                part.range = Range(a, Here());
                part.text = s_.substr(a, Here() - a);
                part.quoted = in_dq;
                return part;
            }
            BashWordPart part;
            part.kind = BashPartKind::CommandSub;
            AdvN(2);
            Stop stop;
            stop.paren = true;
            part.body = ParseList(stop, true);
            SkipNewlines();
            if (C() != ')') {
                if (AtEnd()) Fail(a, a + 2, "unexpected EOF while looking for matching `)'", "unexpected-eof");
                Unexpected();
            }
            Adv();
            part.range = Range(a, Here());
            part.text = s_.substr(a, Here() - a);
            part.quoted = in_dq;
            return part;
        }
        if (n1 == '[') {
            BashWordPart part;
            part.kind = BashPartKind::ArithSub;
            AdvN(2);
            part.arith = ReadArith("]");
            Adv();
            part.range = Range(a, Here());
            part.text = s_.substr(a, Here() - a);
            part.quoted = in_dq;
            return part;
        }
        if (n1 == '{') {
            const char n2 = Ck(2);
            if (IsBlank(n2) || n2 == '\n' || n2 == '|') {
                // ${ list; } and ${| list; }: bash 5.3's command substitution
                // in the current shell.
                BashWordPart part;
                part.kind = BashPartKind::CommandSub;
                AdvN(n2 == '|' ? 3 : 2);
                Stop stop;
                stop.brace = true;
                part.body = ParseList(stop, true);
                SkipNewlines();
                if (C() != '}') Fail(a, a + 2, "unexpected EOF while looking for matching `}'", "unexpected-eof");
                Adv();
                part.range = Range(a, Here());
                part.text = s_.substr(a, Here() - a);
                part.quoted = in_dq;
                return part;
            }
            return ReadParamExpansion(in_dq);
        }
        // $name, $1, $@ ...
        BashWordPart part;
        part.kind = BashPartKind::Param;
        part.quoted = in_dq;
        Adv();
        const size_t na = Here();
        if (IsNameStart(C())) {
            while (IsNameChar(C())) {
                part.name += C();
                Adv();
            }
        } else {
            part.name = std::string(1, C());
            Adv();
        }
        part.name_range = Range(na, Here());
        part.range = Range(a, Here());
        part.text = s_.substr(a, Here() - a);
        return part;
    }

    BashWordPart ReadParamExpansion(bool in_dq) {
        BashWordPart part;
        part.kind = BashPartKind::Param;
        part.braced = true;
        part.quoted = in_dq;
        const size_t a = Here();
        AdvN(2);
        auto special = [](char c) { return c != 0 && std::strchr("@*#?-$!0", c) != nullptr; };
        if (C() == '#' && C1() != '}' && (IsNameStart(C1()) || IsDigit(C1()) || special(C1()))) {
            // ${#x}; but ${#-}, ${#?} etc. still read as the length of $- / $?.
            part.length = true;
            Adv();
        } else if (C() == '!' && C1() != '}' && (IsNameStart(C1()) || IsDigit(C1()) || special(C1()))) {
            part.indirect = true;
            Adv();
        }
        const size_t na = Here();
        if (IsNameStart(C())) {
            while (IsNameChar(C())) {
                part.name += C();
                Adv();
            }
        } else if (IsDigit(C())) {
            while (IsDigit(C())) {
                part.name += C();
                Adv();
            }
        } else if (special(C())) {
            part.name = std::string(1, C());
            Adv();
        }
        part.name_range = Range(na, Here());
        if (!part.name.empty() && C() == '[') {
            Adv();
            part.has_subscript = true;
            const size_t sa = Here();
            part.subscript_arith = ReadArith("]");
            part.subscript = s_.substr(sa, Here() - sa);
            Adv();
        }
        if (part.indirect && (C() == '*' || C() == '@') && C1() == '}') {
            part.op = std::string("!") + C();
            Adv();
        }
        if (C() != '}') {
            static const char *const ops[] = {":-", ":=", ":+", ":?", "##", "%%", "//", "/#", "/%", "^^", ",,",
                                              "~~", ":",  "-",  "=",  "+",  "?",  "#",  "%",  "/",  "^",  ",",
                                              "~",  "@"};
            for (const char *op : ops) {
                const size_t len = std::strlen(op);
                bool match = true;
                for (size_t k = 0; k < len; k++) {
                    if (Ck(static_cast<int>(k)) != op[k]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    part.op = op;
                    AdvN(len);
                    break;
                }
            }
            if (part.op.empty() || part.name.empty()) {
                // Not a form bash knows (`${$x}`, `${a b}`): bash reports a bad
                // substitution when it runs, not when it reads. Read to the brace.
                part.op = part.op.empty() ? "?bad" : part.op;
                part.args.push_back(ReadParamWord(in_dq, 0));
            } else if (part.op[0] == '/') {
                part.args.push_back(ReadParamWord(in_dq, '/'));
                if (C() == '/') {
                    Adv();
                    part.args.push_back(ReadParamWord(in_dq, 0));
                }
            } else if (part.op == ":") {
                part.args.push_back(ReadParamWord(in_dq, ':'));
                if (C() == ':') {
                    Adv();
                    part.args.push_back(ReadParamWord(in_dq, 0));
                }
            } else {
                part.args.push_back(ReadParamWord(in_dq, 0));
            }
        }
        if (C() != '}') Fail(a, a + 2, "unexpected EOF while looking for matching `}'", "unexpected-eof");
        Adv();
        part.range = Range(a, Here());
        part.text = s_.substr(a, Here() - a);
        return part;
    }

    BashWord ReadParamWord(bool in_dq, char extra_stop) {
        BashWord w;
        const size_t a = Here();
        ReadParts(Mode::ParamArg, &w.parts, in_dq, extra_stop);
        w.range = Range(a, Here());
        w.text = s_.substr(a, Here() - a);
        return w;
    }

    BashWordPart ReadBackquote(bool in_dq) {
        BashWordPart part;
        part.kind = BashPartKind::CommandSub;
        part.backquoted = true;
        part.quoted = in_dq;
        const size_t a = Here();
        size_t p = a + 1;
        while (p < end_ && s_[p] != '`') {
            if (s_[p] == '\\') p++;
            p++;
        }
        if (p >= end_) Fail(a, a + 1, "unexpected EOF while looking for matching ``'", "unexpected-eof");
        // Bash reads what is between backquotes when it runs it, so an error
        // in there is not a syntax error of the script; still, parse it for
        // the names it uses.
        const size_t save_end = end_;
        auto save_pending = pending_;
        pending_.clear();
        const int save_depth = depth_;
        end_ = p;
        i_ = a + 1;
        try {
            part.body = ParseList(Stop{}, true);
        } catch (const ParseFailure &) {
            part.body.reset();
        }
        end_ = save_end;
        pending_ = std::move(save_pending);
        depth_ = save_depth;
        i_ = Norm(p + 1);
        part.range = Range(a, p + 1);
        part.text = s_.substr(a, p + 1 - a);
        return part;
    }

    BashWordPart ReadProcessSub() {
        BashWordPart part;
        part.kind = BashPartKind::ProcessSub;
        const size_t a = Here();
        AdvN(2);
        Stop stop;
        stop.paren = true;
        part.body = ParseList(stop, true);
        SkipNewlines();
        if (C() != ')') {
            if (AtEnd()) Fail(a, a + 2, "unexpected EOF while looking for matching `)'", "unexpected-eof");
            Unexpected();
        }
        Adv();
        part.range = Range(a, Here());
        part.text = s_.substr(a, Here() - a);
        return part;
    }

    BashWordPart ReadExtGlob() {
        BashWordPart part;
        part.kind = BashPartKind::ExtGlob;
        const size_t a = Here();
        AdvN(2);
        ReadParts(Mode::ExtGlob, &part.parts, false, 0);
        if (C() != ')') Fail(a, a + 2, "unexpected EOF while looking for matching `)'", "unexpected-eof");
        Adv();
        part.range = Range(a, Here());
        part.text = s_.substr(a, Here() - a);
        return part;
    }

    // --- Arithmetic --------------------------------------------------------

    // Reads an arithmetic expression up to `close` (")" ")" for `))`, "]",
    // or ";"), leaving the cursor on it.
    BashArith ReadArith(const char *close) {
        DepthGuard guard(this);
        BashArith ar;
        std::vector<std::pair<size_t, size_t>> quoted;
        const size_t a = Here();
        const std::string cl = close;
        int depth = 0;
        for (;;) {
            const size_t p = Here();
            const char c = At(p);
            if (p >= end_) {
                const std::string want = cl == "]" ? "]" : ")";
                Fail(a, a, "unexpected EOF while looking for matching `" + want + "'", "unexpected-eof");
            }
            if (c == '(' || (c == '[' && cl == "]")) {
                depth++;
            } else if (c == ')' || (c == ']' && cl == "]")) {
                if (depth == 0) {
                    if (cl == "))" && c == ')' && C1() == ')') break;
                    if (cl == "]" && c == ']') break;
                    if (cl == ";" && c == ')' && C1() == ')') {
                        // for (( a ; b )): fewer than three parts.
                        Fail(p, p + 2, "syntax error: arithmetic expression required");
                    }
                    if (cl == "))" && c == ')') {
                        Fail(p, p + 1, "syntax error near unexpected token `)'");
                    }
                } else {
                    depth--;
                }
            } else if (c == ';' && cl == ";" && depth == 0) {
                break;
            }
            if (c == '$') {
                const char n1 = C1();
                if (n1 == '(' || n1 == '{' || n1 == '[' || IsNameStart(n1) || IsDigit(n1) ||
                    (n1 != 0 && std::strchr("@*#?-$!", n1) != nullptr)) {
                    ar.parts.push_back(ReadDollar(false));
                    continue;
                }
            }
            if (c == '`') {
                ar.parts.push_back(ReadBackquote(false));
                continue;
            }
            if (c == '"') {
                // A quoted string: its expansions count, its text is no name.
                const size_t qa = Here();
                BashWordPart q = ReadDoubleQuoted(BashPartKind::DoubleQuoted, 1);
                for (BashWordPart &inner : q.parts) {
                    if (inner.kind != BashPartKind::Literal) ar.parts.push_back(std::move(inner));
                }
                quoted.emplace_back(qa, Here());
                continue;
            }
            if (c == '\'') {
                ReadSingleQuoted();
                continue;
            }
            if (c == '\\') {
                i_ = Norm(p + 2);
                continue;
            }
            Adv();
        }
        const size_t b = Here();
        ar.range = Range(a, b);
        ar.text = s_.substr(a, b - a);
        ExtractArithRefs(a, b, &ar, quoted);
        return ar;
    }

    void ExtractArithRefs(size_t a, size_t b, BashArith *ar, const std::vector<std::pair<size_t, size_t>> &quoted) {
        // Ranges taken by expansions and quoted strings, which are not names.
        std::vector<std::pair<size_t, size_t>> skip = quoted;
        for (const BashWordPart &p : ar->parts) {
            skip.emplace_back(OffsetOf(p.range.line, p.range.col), OffsetOf(p.range.end_line, p.range.end_col));
        }
        auto skipped = [&](size_t pos) {
            for (const auto &r : skip) {
                if (pos >= r.first && pos < r.second) return r.second;
            }
            return static_cast<size_t>(0);
        };
        size_t p = a;
        while (p < b) {
            if (const size_t to = skipped(p)) {
                p = to;
                continue;
            }
            const char c = s_[p];
            if (c == '\'' ) {
                const size_t q = s_.find('\'', p + 1);
                p = (q == std::string::npos || q >= b) ? b : q + 1;
                continue;
            }
            if (IsDigit(c)) {
                while (p < b && (IsNameChar(s_[p]) || s_[p] == '#' || s_[p] == '@')) p++;
                continue;
            }
            if (IsNameStart(c)) {
                const size_t s = p;
                while (p < b && IsNameChar(s_[p])) p++;
                BashArithRef ref;
                ref.name = s_.substr(s, p - s);
                ref.range = Range(s, p);
                size_t q = p;
                if (q < b && s_[q] == '[') {
                    int depth = 0;
                    while (q < b) {
                        if (s_[q] == '[') depth++;
                        if (s_[q] == ']' && --depth == 0) {
                            q++;
                            break;
                        }
                        q++;
                    }
                }
                while (q < b && (IsBlank(s_[q]) || s_[q] == '\n')) q++;
                if (q + 1 <= b && q < b) {
                    const char d = s_[q];
                    const char e = q + 1 < b ? s_[q + 1] : 0;
                    if (d == '=' && e != '=') ref.assigned = true;
                    if (std::strchr("+-*/%&^|", d) != nullptr && d != 0 && e == '=') ref.assigned = true;
                    if ((d == '+' && e == '+') || (d == '-' && e == '-')) ref.assigned = true;
                    if ((d == '<' && e == '<') || (d == '>' && e == '>')) {
                        if (q + 2 < b && s_[q + 2] == '=') ref.assigned = true;
                    }
                }
                size_t r = s;
                while (r > a && IsBlank(s_[r - 1])) r--;
                if (r >= a + 2 && ((s_[r - 1] == '+' && s_[r - 2] == '+') || (s_[r - 1] == '-' && s_[r - 2] == '-'))) {
                    ref.assigned = true;
                }
                ar->refs.push_back(std::move(ref));
                continue;
            }
            p++;
        }
    }
};

void WalkPart(const BashWordPart &part, BashVisitor &v, const BashCommand *fn);
void WalkCommand(const BashCommand &cmd, BashVisitor &v, const BashCommand *fn);

void WalkArith(const BashArith &ar, BashVisitor &v, const BashCommand *fn) {
    for (const BashArithRef &r : ar.refs) v.ArithRef(r, fn);
    for (const BashWordPart &p : ar.parts) WalkPart(p, v, fn);
}

void WalkWord(const BashWord &w, BashVisitor &v, const BashCommand *fn) {
    for (const BashWordPart &p : w.parts) WalkPart(p, v, fn);
}

void WalkList(const BashList &list, BashVisitor &v, const BashCommand *fn) {
    for (const BashAndOr &ao : list.items) {
        for (const BashPipeline &pl : ao.pipelines) {
            for (const BashCommand &c : pl.commands) WalkCommand(c, v, fn);
        }
    }
}

void WalkPart(const BashWordPart &part, BashVisitor &v, const BashCommand *fn) {
    v.Part(part, fn);
    for (const BashWordPart &p : part.parts) WalkPart(p, v, fn);
    for (const BashWord &w : part.args) WalkWord(w, v, fn);
    if (part.body) WalkList(*part.body, v, fn);
    if (part.kind == BashPartKind::ArithSub) WalkArith(part.arith, v, fn);
    if (part.has_subscript) WalkArith(part.subscript_arith, v, fn);
}

void WalkCommand(const BashCommand &cmd, BashVisitor &v, const BashCommand *fn) {
    if (!v.Command(cmd, fn)) return;
    const BashCommand *inner = cmd.kind == BashCommandKind::Function ? &cmd : fn;
    for (const BashAssignment &a : cmd.assignments) {
        if (a.declaration) continue;  // also in `words`
        WalkWord(a.value, v, fn);
        for (const BashWord &w : a.elements) WalkWord(w, v, fn);
        if (a.has_subscript) WalkArith(a.subscript_arith, v, fn);
    }
    for (const BashAssignment &a : cmd.assignments) {
        if (!a.declaration) continue;
        for (const BashWord &w : a.elements) WalkWord(w, v, fn);
        if (a.has_subscript) WalkArith(a.subscript_arith, v, fn);
    }
    for (const BashWord &w : cmd.words) WalkWord(w, v, fn);
    for (const BashRedirect &r : cmd.redirects) {
        if (!r.heredoc) WalkWord(r.target, v, fn);
        for (const BashWordPart &p : r.body_parts) WalkPart(p, v, fn);
    }
    WalkWord(cmd.subject, v, fn);
    for (const BashWord &w : cmd.items) WalkWord(w, v, fn);
    for (const BashArith &ar : cmd.ariths) WalkArith(ar, v, fn);
    for (const BashCondTerm &t : cmd.cond) {
        if (t.kind == BashCondTerm::Word) WalkWord(t.word, v, fn);
    }
    for (const auto &b : cmd.bodies) {
        if (b) WalkList(*b, v, inner);
    }
    for (const BashCaseItem &ci : cmd.case_items) {
        for (const BashWord &w : ci.patterns) WalkWord(w, v, fn);
        if (ci.body) WalkList(*ci.body, v, inner);
    }
    for (const BashCommand &s : cmd.sub) WalkCommand(s, v, inner);
}

}  // namespace

bool BashRangeContains(const BashRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col >= r.end_col) return false;
    return true;
}

bool BashRangeTouches(const BashRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col > r.end_col) return false;
    return true;
}

bool BashWordLiteral(const BashWord &word, std::string *out) {
    std::string s;
    for (const BashWordPart &p : word.parts) {
        switch (p.kind) {
            case BashPartKind::Literal:
                for (size_t k = 0; k < p.text.size(); k++) {
                    if (p.text[k] == '\\' && k + 1 < p.text.size()) {
                        if (p.text[k + 1] == '\n') {
                            k++;
                            continue;
                        }
                        if (!p.quoted || std::strchr("$`\"\\", p.text[k + 1]) != nullptr) k++;
                    }
                    s += p.text[k];
                }
                break;
            case BashPartKind::SingleQuoted:
                s += p.text.substr(1, p.text.size() - 2);
                break;
            case BashPartKind::DoubleQuoted: {
                BashWord inner;
                inner.parts = p.parts;
                std::string t;
                if (!BashWordLiteral(inner, &t)) return false;
                s += t;
                break;
            }
            default:
                return false;
        }
    }
    *out = s;
    return true;
}

bool BashIsReservedWord(const std::string &w) {
    static const char *const words[] = {"if",   "then", "else",   "elif", "fi", "case", "esac", "for",
                                        "select", "while", "until", "do",   "done", "in",   "function",
                                        "time", "{",    "}",      "!",    "[[",   "]]",   "coproc"};
    for (const char *k : words) {
        if (w == k) return true;
    }
    return false;
}

bool BashIsName(const std::string &s) {
    if (s.empty() || !IsNameStart(s[0])) return false;
    for (char c : s) {
        if (!IsNameChar(c)) return false;
    }
    return true;
}

BashParseResult BashParse(const std::vector<std::string> &lines) {
    Parser p(lines);
    return p.Run();
}

void BashWalk(const BashList &list, BashVisitor &visitor) { WalkList(list, visitor, nullptr); }
