// mep's Octave formatter: see octave_format.h.
//
// The layout is MISS_HIT's `mh_style --fix` (miss_hit_core 0.9.44): a
// token-stream rewrite, not a pretty-printer. Every line keeps its tokens
// and the gaps between them, except where a rule says otherwise:
//
//   - indentation: `tab_width` (4) per block level -- function bodies,
//     if/elseif/else, loops, try/catch, switch (and once more for each case),
//     classdef blocks; comments follow the code around them; continuation
//     lines align with the bracket they are inside, or keep their offset
//     from the statement's first token;
//   - whitespace: one blank around binary operators, '=' and after ',' ';'
//     and keywords; none inside brackets, around ':' and '^' '.^', after a
//     unary operator; `%comment` becomes `% comment`;
//   - statements: one per line (`if x, y = 1; end` is split), extra ';' and
//     ',' removed, the comma/semicolon a matrix row does not need removed,
//     redundant brackets removed (`if (x)`, `f((a))`);
//   - blank lines: at most one in a row, one around each function, none at
//     the start; trailing whitespace and tabs (expanded) gone; one final
//     newline.
//
// MISS_HIT's own reader is replaced by mep's (octave_parse.h), so a file is
// formatted only when Octave would accept it, and Octave's own constructs
// (endif, do/until, unwind_protect, # comments) are laid out like their
// MATLAB counterparts. The token boundaries, the rules and the replay are
// MISS_HIT's (m_lexer.py's MATLAB_Lexer and Token_Buffer.replay,
// mh_style.py's stage_3_analysis, m_parser.py's match_eos /
// parse_row_list / check_redundant_brackets), re-implemented here.
//
// Deliberate differences: MISS_HIT ends every statement with ';' (which
// changes what Octave prints), so that is `Options::add_semicolons`, off by
// default; with it off a ',' ending a statement is removed instead of
// turned into ';'. MISS_HIT is not idempotent (continuation lines align with
// the columns before the line's own spacing was fixed), so by default the
// formatter runs to a fixed point (`Options::converge`). When a redundant
// ')' MISS_HIT deletes was where it meant to break the line
// (`if (x), y = 1; end`), the break moves to the token before it instead of
// being lost (MISS_HIT writes `if x y = 1;`).
#include "octave_format.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "octave_parse.h"

namespace octavefmt {

namespace {

using octparse::AstNode;
using octparse::Node;
using octparse::ParseResult;
using octparse::Tok;
using octparse::Token;

enum class K {
    Newline, Comment, Continuation, Identifier, Keyword, Number, Carray, String, Comma, Semicolon, Colon,
    Assignment, Operator, Bra, Ket, MBra, MKet, CBra, CKet, ABra, AKet, Selection, At, Metaclass,
};

struct Fix {
    bool del = false;
    bool ws_before = false, ws_after = false, trim_before = false, trim_after = false;
    bool maxgap_before = false, maxgap_after = false;
    bool add_newline = false, add_semicolon_after = false, change_to_semicolon = false, replace_with_newline = false;
    bool spurious = false, statement_terminator = false, flag_continuations = false;
    bool unary = false, binary = false, postfix = false;
    int correct_indent = -1;
};

struct T {
    K kind = K::Newline;
    std::string raw, value;
    int line = 1, cs = 0, ce = 0;  // 1-based line, 0-based inclusive columns
    int begin = 0;                 // byte offset
    bool fil = false, fis = false, anon = false, block = false;
    bool continued = false;  // a NEWLINE ending a comment line inside a `...` statement
    int mt = -1;  // mep token index
    // the AST link: get_indentation(), causes_indentation(), "the end of a function"
    bool linked = false, causes = false, fn_end = false;
    int level = 0;
    Fix fix;
};

bool IsKet(K k) { return k == K::Ket || k == K::MKet || k == K::CKet; }
bool IsAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

bool IsEndKeyword(const std::string &w) {
    return w == "end" || w == "endif" || w == "endfor" || w == "endparfor" || w == "endwhile" ||
           w == "endswitch" || w == "endfunction" || w == "end_try_catch" || w == "end_unwind_protect" ||
           w == "endclassdef" || w == "endproperties" || w == "endmethods" || w == "endevents" ||
           w == "endenumeration" || w == "endspmd" || w == "endarguments";
}

std::string Rstrip(const std::string &s) {
    size_t e = s.size();
    while (e > 0 && std::strchr(" \t\n\r\f\v", s[e - 1])) e--;
    return s.substr(0, e);
}
std::string Strip(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::strchr(" \t\n\r\f\v", s[b])) b++;
    return Rstrip(s.substr(b));
}

// ---------------------------------------------------------------------------------------------
// The lexer: MISS_HIT's token stream (NEWLINE runs, COMMENT, CONTINUATION, block comments,
// anonymous commas in matrices, A_BRA for `[a, b] =`), with the code tokens' extents and
// kinds taken from mep's own reader, which knows Octave's string/transpose and command
// syntax rules.

class Lexer {
public:
    Lexer(const std::string &s, const ParseResult &p) : s_(s), p_(p) {
        for (size_t i = 0; i < p.tokens.size(); i++) {
            const Token &t = p.tokens[i];
            if (t.synthetic || t.kind == Tok::Eof || t.kind == Tok::Newline || t.end <= t.begin) continue;
            tok_at_.emplace(t.begin, static_cast<int>(i));
        }
        for (const AstNode &n : p.nodes)
            if (n.kind == Node::Command && n.tok >= 0) command_names_.insert(n.tok);
    }

    bool Run(std::vector<T> *out, std::string *err) {
        for (;;) {
            T t;
            const int r = Next(&t);
            if (r < 0) {
                *err = err_;
                return false;
            }
            if (r == 0) break;
            out->push_back(std::move(t));
        }
        // A top-level [ ... ] followed (past continuations) by '=' is an assignment list.
        for (size_t i = 0; i < out->size(); i++) {
            if ((*out)[i].kind != K::MBra || !top_level_open_.count(static_cast<int>(i))) continue;
            int depth = 0;
            size_t j = i;
            for (; j < out->size(); j++) {
                const K k = (*out)[j].kind;
                if (k == K::Bra || k == K::MBra || k == K::CBra) depth++;
                if (IsKet(k) && --depth == 0) break;
            }
            if (j >= out->size()) continue;
            size_t a = j + 1;
            while (a < out->size() && (*out)[a].kind == K::Continuation) a++;
            if (a < out->size() && (*out)[a].kind == K::Assignment) {
                (*out)[i].kind = K::ABra;
                (*out)[j].kind = K::AKet;
            }
        }
        return true;
    }

private:
    const std::string &s_;
    const ParseResult &p_;
    std::map<int, int> tok_at_;
    std::set<int> command_names_;
    std::set<int> top_level_open_;
    long lexpos_ = -1;
    long col_offset_ = 0;
    int line_ = 1;
    int cont_chain_ = 0;  // 1 after a `...` (or a continued NEWLINE), 2 after a comment line there
    bool fil_ = true, fis_ = true, add_comma_ = false, in_lambda_ = false, command_mode_ = false;
    int block_comment_ = 0;
    std::vector<K> bstack_;
    bool have_last_ = false;
    K last_kind_ = K::Newline;
    std::string err_;
    int out_count_ = 0;

    char At(long i) const { return i >= 0 && static_cast<size_t>(i) < s_.size() ? s_[static_cast<size_t>(i)] : '\0'; }
    char Cc() const { return At(lexpos_); }
    char Nc() const { return At(lexpos_ + 1); }
    char Nnc() const { return At(lexpos_ + 2); }
    char Nnnc() const { return At(lexpos_ + 3); }
    void Skip() {
        const char old = At(lexpos_);
        lexpos_++;
        if (old == '\n') col_offset_ = lexpos_;
    }
    static bool IsCommentChar(char c) { return c == '%' || c == '#'; }

    int Error(const std::string &m) {
        err_ = m + " (line " + std::to_string(line_) + ")";
        return -1;
    }

    // 1: a token, 0: end of input, -1: error
    int Next(T *tok) {
        if (add_comma_) {
            add_comma_ = false;
            tok->kind = K::Comma;
            tok->raw = tok->value = ",";
            tok->line = line_;
            tok->cs = static_cast<int>(lexpos_ - col_offset_ + 1);
            tok->ce = tok->cs + 6;
            tok->begin = static_cast<int>(lexpos_ + 1);
            tok->anon = true;
            have_last_ = true;
            last_kind_ = K::Comma;
            out_count_++;
            return 1;
        }
        if (!block_comment_) {
            for (;;) {
                Skip();
                if (Cc() != ' ' && Cc() != '\t') break;
            }
        } else {
            Skip();
        }
        if (static_cast<size_t>(lexpos_) >= s_.size()) return 0;
        const long t_start = lexpos_;
        const int col_start = static_cast<int>(t_start - col_offset_);
        K kind = K::Newline;
        bool quotes = false;
        int mt = -1;

        if (block_comment_) {
            if (Cc() == '\n') {
                kind = K::Newline;
            } else {
                kind = K::Comment;
                while (Nc() != '\n' && Nc() != '\0') Skip();
            }
        } else if (command_mode_ && !tok_at_.count(static_cast<int>(t_start))) {
            if (IsCommentChar(Cc())) {
                kind = K::Comment;
                while (Nc() != '\n' && Nc() != '\0') Skip();
            } else if (Cc() == '\n') {
                kind = K::Newline;
                while (Nc() == '\n' || Nc() == ' ' || Nc() == '\t') Skip();
            } else if (Cc() == ';') {
                kind = K::Semicolon;
            } else if (Cc() == ',') {
                kind = K::Comma;
            } else if (Cc() == '.' && Nc() == '.' && Nnc() == '.') {
                kind = K::Continuation;
                while (Cc() != '\n' && Cc() != '\0') Skip();
            } else {
                kind = K::Carray;
                int local = 0;
                bool str = Cc() == '\'';
                for (;;) {
                    if (str) {
                        if (Nc() == '\'' && Nnc() == '\'') {
                            Skip();
                        } else if (Nc() == '\'') {
                            str = false;
                        } else if (Nc() == '\0' || Nc() == '\n') {
                            break;
                        }
                    } else {
                        if (Cc() == '(' || Cc() == '{' || Cc() == '[') local++;
                        else if (Cc() == ')' || Cc() == '}' || Cc() == ']') local--;
                        if (Nc() == '.' && Nnc() == '.' && Nnnc() == '.') break;
                        if (IsCommentChar(Nc())) break;
                        if (Nc() == '\n' || Nc() == '\0') break;
                        if (local == 0 && (Nc() == ' ' || Nc() == '\t' || Nc() == ',' || Nc() == ';')) break;
                        if (Nc() == '\'') str = true;
                    }
                    Skip();
                }
            }
        } else if (tok_at_.count(static_cast<int>(t_start))) {
            mt = tok_at_[static_cast<int>(t_start)];
            const Token &m = p_.tokens[static_cast<size_t>(mt)];
            lexpos_ = m.end - 1;
            switch (m.kind) {
            case Tok::Name:
                kind = m.text == "end" ? K::Keyword : K::Identifier;
                break;
            case Tok::StructElt:
            case Tok::FcnHandle:
            case Tok::MetaQuery:
            case Tok::SuperclassRef:
            case Tok::FqIdent:
                kind = K::Identifier;
                break;
            case Tok::Keyword:
                kind = K::Keyword;
                break;
            case Tok::Number:
                kind = K::Number;
                break;
            case Tok::SqString:
                kind = K::Carray;
                quotes = s_[static_cast<size_t>(m.begin)] == '\'';
                break;
            case Tok::DqString:
                kind = K::String;
                quotes = true;
                break;
            case Tok::Comma:
                kind = K::Comma;
                break;
            case Tok::Semi:
                kind = K::Semicolon;
                break;
            case Tok::Op: {
                const std::string &o = m.text;
                if (o == "=") kind = K::Assignment;
                else if (o.size() == 2 && o[1] == '=' && std::strchr("+-*/^|&", o[0])) kind = K::Assignment;
                else if (o == ",") kind = K::Comma;
                else if (o == ";") kind = K::Semicolon;
                else if (o == ":") kind = K::Colon;
                else if (o == "(") kind = K::Bra;
                else if (o == ")") kind = K::Ket;
                else if (o == "[") kind = K::MBra;
                else if (o == "]") kind = K::MKet;
                else if (o == "{") kind = K::CBra;
                else if (o == "}") kind = K::CKet;
                else if (o == ".") kind = K::Selection;
                else if (o == "@") kind = K::At;
                else if (o == "?") kind = K::Metaclass;
                else kind = K::Operator;
                break;
            }
            default:
                return Error("unexpected token");
            }
        } else if (IsCommentChar(Cc())) {
            kind = K::Comment;
            while (Nc() != '\n' && Nc() != '\0') Skip();
        } else if (Cc() == '\n') {
            kind = K::Newline;
            while (Nc() == '\n' || Nc() == ' ' || Nc() == '\t') Skip();
        } else if (Cc() == ';') {
            kind = K::Semicolon;
        } else if (Cc() == ',') {
            kind = K::Comma;
        } else if (Cc() == '.' && Nc() == '.' && Nnc() == '.') {
            kind = K::Continuation;
            Skip();
            Skip();
            while (Cc() != '\n' && Cc() != '\0') Skip();
        } else if (Cc() == '\r' || Cc() == '\f' || Cc() == '\v') {
            return Error("unexpected control character");
        } else {
            return Error(std::string("unexpected character '") + Cc() + "'");
        }

        const long t_end = std::min(lexpos_, static_cast<long>(s_.size()) - 1);
        tok->kind = kind;
        tok->raw = s_.substr(static_cast<size_t>(t_start), static_cast<size_t>(t_end - t_start + 1));
        tok->line = line_;
        tok->cs = col_start;
        tok->ce = static_cast<int>(t_end - col_offset_);
        tok->begin = static_cast<int>(t_start);
        tok->fil = fil_;
        tok->fis = fis_;
        tok->block = block_comment_ > 0;
        tok->mt = mt;
        // MATLAB_Token's value
        if (kind == K::Continuation) tok->value = Strip(tok->raw.substr(3));
        else if (kind == K::Comment) tok->value = tok->block ? Strip(tok->raw) : Strip(tok->raw.substr(1));
        else if ((kind == K::Carray || kind == K::String) && quotes && tok->raw.size() >= 2)
            tok->value = tok->raw.substr(1, tok->raw.size() - 2);
        else tok->value = tok->raw;
        fil_ = false;
        fis_ = false;

        if (kind == K::Bra && have_last_ && last_kind_ == K::At) in_lambda_ = true;
        if (kind == K::Newline) {
            line_ += static_cast<int>(std::count(tok->raw.begin(), tok->raw.end(), '\n'));
            fil_ = true;
        } else if (kind == K::Continuation) {
            line_ += 1;
            fil_ = true;
        }
        if (mt >= 0 && command_names_.count(mt)) command_mode_ = true;
        // Octave reads on through a comment line after `...`: the line after it still
        // continues the statement.
        const bool continued = kind == K::Newline && cont_chain_ == 2 &&
                               std::count(tok->raw.begin(), tok->raw.end(), '\n') == 1;
        tok->continued = continued;
        cont_chain_ = kind == K::Continuation || continued ? 1 : cont_chain_ > 0 && kind == K::Comment && tok->fil ? 2 : 0;
        if (bstack_.empty() && !continued) {
            if (kind == K::Newline || kind == K::Comma || kind == K::Semicolon ||
                (kind == K::Keyword && tok->raw == "try")) {
                fis_ = true;
                command_mode_ = false;
            }
        }
        if (kind == K::Comment) {
            const bool opens = block_comment_ == 0 ? tok->raw.compare(1, 1, "{") == 0
                                                   : (tok->raw.find("%{") != std::string::npos ||
                                                      tok->raw.find("#{") != std::string::npos);
            if (opens) {
                if (!tok->fil && block_comment_ == 0) {
                } else if (tok->value != "{" && block_comment_ == 0) {
                } else if (Strip(tok->raw) != "%{" && Strip(tok->raw) != "#{") {
                } else {
                    block_comment_++;
                    tok->block = true;
                }
            } else if (block_comment_) {
                for (const char *marker : {"%}", "#}"}) {
                    if (tok->raw.find(marker) != std::string::npos && Strip(tok->raw) == marker) {
                        block_comment_--;
                        tok->value = "}";
                    }
                }
            }
        }
        have_last_ = true;
        last_kind_ = kind;

        if (kind == K::Bra || kind == K::MBra || kind == K::CBra) {
            if (kind == K::MBra && bstack_.empty()) top_level_open_.insert(out_count_);
            bstack_.push_back(kind);
        } else if (IsKet(kind)) {
            if (!bstack_.empty()) bstack_.pop_back();
        }

        // Anonymous commas: whitespace between elements of a matrix or a cell.
        const bool ws_significant = !bstack_.empty() && (bstack_.back() == K::MBra || bstack_.back() == K::CBra) &&
                                    !(kind == K::MBra || kind == K::CBra);
        bool ws_follows = Nc() == ' ' || Nc() == '\t';
        bool skip_cont = false;
        char next_non_ws = 0, after_next = 0;
        for (size_t n = static_cast<size_t>(lexpos_ + 1); n < s_.size(); n++) {
            const char c = s_[n];
            if (skip_cont && c == '\n') {
                skip_cont = false;
            } else if (skip_cont) {
            } else if ((c == ' ' || c == '\t') && !next_non_ws) {
            } else if (s_.compare(n, 3, "...") == 0 && !next_non_ws) {
                skip_cont = true;
                ws_follows = true;
            } else if (!next_non_ws) {
                next_non_ws = c;
            } else {
                after_next = c;
                break;
            }
        }
        bool relevant = kind == K::Identifier || kind == K::Number || kind == K::Carray || kind == K::String ||
                        IsKet(kind) || (kind == K::Keyword && tok->raw == "end") ||
                        (kind == K::Operator && (tok->raw == "'" || tok->raw == ".'"));
        if (next_non_ws && after_next) {
            if (next_non_ws == '.' && IsDigit(after_next)) {
            } else if (std::strchr("*/\\^<>&|=.:!", next_non_ws)) {
                relevant = false;
            } else if (next_non_ws == '~' && after_next == '=') {
                relevant = false;
            }
        }
        if (ws_significant && ws_follows && next_non_ws && relevant) {
            const char c = next_non_ws;
            if (c == ',' || c == ';' || c == '\n' || IsCommentChar(c)) {
            } else if (IsAlnum(c) || c == '\'' || c == '"' || std::strchr("([{@?.", c)) {
                add_comma_ = true;
            } else if (std::strchr("-+~", c)) {
                if (after_next && (std::strchr("+-([.~", after_next) || IsAlnum(after_next))) add_comma_ = true;
            }
        }
        if (in_lambda_ && kind == K::Ket) {
            add_comma_ = false;
            in_lambda_ = false;
        }
        out_count_++;
        return 1;
    }
};

// ---------------------------------------------------------------------------------------------
// What MISS_HIT's parser contributes: the AST links (indentation levels), unary/binary
// operators, the statement terminators to fix, the spurious row separators, the redundant
// brackets.

struct EosSpec {
    int header_end = -1;  // mep token: the last token of what the terminators end
    bool semi = false;
    bool allow_nothing = false;
    int level = 0;
    bool causes = false;
};

class Analyzer {
public:
    Analyzer(const ParseResult &p, std::vector<T> &toks, const Options &o) : p_(p), t_(toks), opt_(o) {
        lvl_.assign(p.tokens.size(), 0);
        causes_.assign(p.tokens.size(), false);
        fn_end_.assign(p.tokens.size(), false);
        for (size_t i = 0; i < t_.size(); i++)
            if (t_[i].mt >= 0) mh_of_.emplace(t_[i].mt, static_cast<int>(i));
        // the parser's view: no comments or continuations, no NEWLINE straight after a NEWLINE
        for (size_t i = 0; i < t_.size(); i++) {
            const K k = t_[i].kind;
            if (k == K::Comment || k == K::Continuation || t_[i].continued) continue;
            if (k == K::Newline && !view_.empty() && t_[static_cast<size_t>(view_.back())].kind == K::Newline)
                continue;
            view_pos_.emplace(static_cast<int>(i), static_cast<int>(view_.size()));
            view_.push_back(static_cast<int>(i));
        }
    }

    void Run() {
        const AstNode &root = N(p_.root);
        Walk(root, 0);
        // Tokens on the AST: the mep tokens' links; command words follow their command name.
        int last_level = 0;
        bool have = false;
        for (T &t : t_) {
            if (t.mt >= 0) {
                t.linked = true;
                t.level = lvl_[static_cast<size_t>(t.mt)];
                t.causes = causes_[static_cast<size_t>(t.mt)];
                t.fn_end = fn_end_[static_cast<size_t>(t.mt)];
                last_level = t.level;
                have = true;
            } else if (t.kind == K::Carray && have) {
                t.linked = true;
                t.level = last_level;
            }
        }
        Operators();
        for (const EosSpec &e : eos_) MatchEos(e);
        Rows();
        Brackets();
    }

private:
    const ParseResult &p_;
    std::vector<T> &t_;
    const Options &opt_;
    std::vector<int> lvl_;
    std::vector<bool> causes_, fn_end_;
    std::map<int, int> mh_of_;
    std::vector<int> view_;
    std::map<int, int> view_pos_;
    std::vector<EosSpec> eos_;

    const AstNode &N(int i) const { return p_.nodes[static_cast<size_t>(i)]; }
    const Token &TK(int i) const { return p_.tokens[static_cast<size_t>(i)]; }
    bool IsSep(int i) const {
        const Token &t = TK(i);
        return t.synthetic || t.kind == Tok::Newline || t.kind == Tok::Comma || t.kind == Tok::Semi ||
               t.kind == Tok::Eof || (t.kind == Tok::Op && (t.text == "," || t.text == ";"));
    }
    void Fill(int first, int last, int level, bool causes) {
        if (first < 0 || last < first) return;
        for (int i = first; i <= last && static_cast<size_t>(i) < lvl_.size(); i++) {
            lvl_[static_cast<size_t>(i)] = level;
            causes_[static_cast<size_t>(i)] = causes;
        }
    }
    void Set(int tok, int level, bool causes) {
        if (tok < 0 || static_cast<size_t>(tok) >= lvl_.size()) return;
        lvl_[static_cast<size_t>(tok)] = level;
        causes_[static_cast<size_t>(tok)] = causes;
    }
    // The last non-separator token in [from, to).
    int HeaderEnd(int from, int to) const {
        int last = -1;
        for (int i = from; i < to && static_cast<size_t>(i) < p_.tokens.size(); i++)
            if (!IsSep(i)) last = i;
        return last;
    }
    int FirstTok(int node) const {
        if (node < 0) return -1;
        return N(node).first;
    }
    void Eos(int header_end, bool semi, bool allow_nothing, int level, bool causes) {
        if (header_end < 0) return;
        EosSpec e;
        e.header_end = header_end;
        e.semi = semi;
        e.allow_nothing = allow_nothing;
        e.level = level;
        e.causes = causes;
        eos_.push_back(e);
    }
    // The first token of a body (a StatementList), or `fallback`.
    int BodyStart(int body, int fallback) const {
        if (body >= 0 && N(body).first >= 0 && !N(body).kids.empty()) return N(N(body).kids[0]).first;
        return fallback;
    }
    // The keyword token with this text in [from, to] not inside any of `skip`'s ranges.
    int FindKw(int from, int to, const char *w) const {
        for (int i = from; i <= to && i >= 0 && static_cast<size_t>(i) < p_.tokens.size(); i++)
            if (TK(i).kind == Tok::Keyword && TK(i).text == w) return i;
        return -1;
    }
    int EndTok(const AstNode &n) const {
        if (n.last >= 0 && TK(n.last).kind == Tok::Keyword && IsEndKeyword(TK(n.last).text)) return n.last;
        return -1;
    }

    void WalkList(int list, int level) {
        if (list < 0) return;
        for (int k : N(list).kids) Walk(N(k), level, k);
    }
    void Walk(const AstNode &n, int level, int self = -1) {
        (void)self;
        switch (n.kind) {
        case Node::StatementList:
            for (int k : n.kids) Walk(N(k), level, k);
            return;
        case Node::If: {
            Fill(n.first, n.last, level + 1, false);
            for (size_t c = 0; c < n.kids.size(); c++) {
                const AstNode &cl = N(n.kids[c]);
                const int next = c + 1 < n.kids.size() ? N(n.kids[c + 1]).first : (EndTok(n) >= 0 ? EndTok(n) : n.last + 1);
                if (cl.kind == Node::IfClause) {
                    Set(cl.tok, level, true);
                    const int body = cl.kids.size() > 1 ? cl.kids[1] : -1;
                    const int bs = BodyStart(body, next);
                    Eos(HeaderEnd(cl.tok, bs), false, true, level, true);
                    WalkList(body, level + 1);
                } else {
                    Set(cl.first, level, true);
                    const int body = cl.kids.empty() ? -1 : cl.kids[0];
                    const int bs = BodyStart(body, next);
                    Eos(HeaderEnd(cl.first, bs), false, true, level, true);
                    WalkList(body, level + 1);
                }
            }
            const int e = EndTok(n);
            if (e >= 0) {
                Set(e, level, false);
                Eos(e, false, false, level, false);
            }
            return;
        }
        case Node::Switch: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            const int first_case = n.kids.size() > 1 ? N(n.kids[1]).first : (EndTok(n) >= 0 ? EndTok(n) : n.last + 1);
            Eos(HeaderEnd(n.first, first_case), false, false, level, true);
            for (size_t c = 1; c < n.kids.size(); c++) {
                const AstNode &cl = N(n.kids[c]);
                const int next = c + 1 < n.kids.size() ? N(n.kids[c + 1]).first : (EndTok(n) >= 0 ? EndTok(n) : n.last + 1);
                Fill(cl.first, cl.last, level + 2, false);
                Set(cl.first, level + 1, true);
                const int body = cl.kind == Node::Case ? (cl.kids.size() > 1 ? cl.kids[1] : -1) : (cl.kids.empty() ? -1 : cl.kids[0]);
                const int bs = BodyStart(body, next);
                Eos(HeaderEnd(cl.first, bs), false, true, level + 1, true);
                WalkList(body, level + 2);
            }
            const int e = EndTok(n);
            if (e >= 0) {
                Set(e, level, true);
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::While:
        case Node::For:
        case Node::Spmd: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            const int body = n.kind == Node::While ? (n.kids.size() > 1 ? n.kids[1] : -1)
                             : n.kind == Node::For ? (n.kids.size() > 2 ? n.kids[2] : -1)
                                                   : (n.kids.empty() ? -1 : n.kids[0]);
            const int e = EndTok(n);
            const int bs = BodyStart(body, e >= 0 ? e : n.last + 1);
            const bool parfor = n.kind == Node::For && TK(n.first).text == "parfor";
            Eos(HeaderEnd(n.first, bs), false, n.kind == Node::For && !parfor, level, true);
            WalkList(body, level + 1);
            if (e >= 0) {
                Set(e, level, true);
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::DoUntil: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            const int body = n.kids.empty() ? -1 : n.kids[0];
            const int cond = n.kids.size() > 1 ? n.kids[1] : -1;
            const int until = cond >= 0 ? FindKw(N(body).last >= 0 ? N(body).last : n.first, N(cond).first, "until") : -1;
            const int bs = BodyStart(body, until >= 0 ? until : n.last + 1);
            Eos(HeaderEnd(n.first, bs), false, true, level, true);
            WalkList(body, level + 1);
            if (until >= 0) {
                Set(until, level, false);
                Eos(HeaderEnd(until, n.last + 1), false, false, level, false);
            }
            return;
        }
        case Node::Try:
        case Node::Unwind: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            const int body = n.kids.empty() ? -1 : n.kids[0];
            const int e = EndTok(n);
            int second = -1, second_body = -1;
            if (n.kind == Node::Try && n.kids.size() > 1) {
                second = N(n.kids[1]).first;
                second_body = N(n.kids[1]).kids.empty() ? -1 : N(n.kids[1]).kids[0];
            } else if (n.kind == Node::Unwind && n.kids.size() > 1) {
                second = FindKw(n.first, n.last, "unwind_protect_cleanup");
                second_body = n.kids[1];
            }
            const int bs = BodyStart(body, second >= 0 ? second : (e >= 0 ? e : n.last + 1));
            Eos(HeaderEnd(n.first, bs), false, true, level, true);
            WalkList(body, level + 1);
            if (second >= 0) {
                Set(second, level, true);
                const int bs2 = BodyStart(second_body, e >= 0 ? e : n.last + 1);
                Eos(HeaderEnd(second, bs2), false, n.kind == Node::Unwind, level, true);
                WalkList(second_body, level + 1);
            }
            if (e >= 0) {
                Set(e, level, true);
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::Function: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            const int body = n.kids.size() > 2 ? n.kids[2] : -1;
            const int e = n.explicit_end ? EndTok(n) : -1;
            int bs = -1;
            if (body >= 0) {
                for (int k : N(body).kids)
                    if (N(k).first >= 0) {
                        bs = N(k).first;
                        break;
                    }
            }
            if (bs < 0) bs = e >= 0 ? e : n.last + 1;
            Eos(HeaderEnd(n.first, bs), false, false, level + 1, false);
            if (body >= 0) {
                for (int k : N(body).kids) {
                    const AstNode &st = N(k);
                    // A function after an end-less one is its sibling, not nested.
                    if (st.kind == Node::Function && !n.explicit_end) Walk(st, level, k);
                    else Walk(st, level + 1, k);
                }
            }
            if (e >= 0) {
                Set(e, level, true);
                fn_end_[static_cast<size_t>(e)] = true;
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::Classdef: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            int first_block = -1;
            for (int k : n.kids)
                if (N(k).kind == Node::ClassBlock) {
                    first_block = N(k).first;
                    break;
                }
            const int e = EndTok(n);
            Eos(HeaderEnd(n.first, first_block >= 0 ? first_block : (e >= 0 ? e : n.last + 1)), false, false, level,
                true);
            for (int k : n.kids) {
                const AstNode &b = N(k);
                if (b.kind != Node::ClassBlock) continue;
                Fill(b.first, b.last, level + 2, false);
                Set(b.first, level + 1, true);
                const int be = EndTok(b);
                int first_item = -1;
                for (int it : b.kids)
                    if (N(it).kind != Node::Attr && N(it).first >= 0) {
                        first_item = N(it).first;
                        break;
                    }
                Eos(HeaderEnd(b.first, first_item >= 0 ? first_item : (be >= 0 ? be : b.last + 1)), false, false,
                    level + 1, true);
                for (int it : b.kids) {
                    const AstNode &item = N(it);
                    if (item.kind == Node::Function) {
                        Walk(item, level + 2, it);
                    } else if (item.kind != Node::Attr && item.first >= 0) {
                        Fill(item.first, item.last, level + 2, false);
                        Eos(HeaderEnd(item.first, item.last + 1), false, false, level + 2, false);
                    }
                }
                if (be >= 0) {
                    Set(be, level + 1, true);
                    Eos(be, false, false, level + 1, true);
                }
            }
            if (e >= 0) {
                Set(e, level, true);
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::ArgumentsBlock: {
            Fill(n.first, n.last, level + 1, false);
            Set(n.first, level, true);
            for (int k : n.kids) {
                const AstNode &a = N(k);
                if (a.first >= 0) Eos(HeaderEnd(a.first, a.last + 1), false, false, level + 1, false);
            }
            const int e = EndTok(n);
            if (e >= 0) {
                Set(e, level, true);
                Eos(e, false, false, level, true);
            }
            return;
        }
        case Node::ExprStmt:
        case Node::Assign:
        case Node::Command: {
            Fill(n.first, n.last, level, false);
            Eos(HeaderEnd(n.first, n.last + 1), true, false, level, false);
            return;
        }
        case Node::Decl: {
            Fill(n.first, n.last, level, false);
            Eos(HeaderEnd(n.first, n.last + 1), false, TK(n.first).text == "global", level, false);
            return;
        }
        default:
            Fill(n.first, n.last, level, false);
            Eos(HeaderEnd(n.first, n.last + 1), false, false, level, false);
            return;
        }
    }

    void Operators() {
        for (const AstNode &n : p_.nodes) {
            if (n.op < 0) continue;
            auto it = mh_of_.find(n.op);
            if (it == mh_of_.end()) continue;
            T &t = t_[static_cast<size_t>(it->second)];
            if (t.kind != K::Operator) continue;
            if (n.kind == Node::Prefix || n.kind == Node::Postfix) t.fix.unary = true;
            if (n.kind == Node::Postfix) t.fix.postfix = true;
            else if (n.kind == Node::Binary) t.fix.binary = true;
        }
    }

    // m_parser.py's match_eos.
    void MatchEos(const EosSpec &e) {
        auto it = mh_of_.find(e.header_end);
        if (it == mh_of_.end()) return;
        int vi = -1;
        {
            auto v = view_pos_.find(it->second);
            if (v == view_pos_.end()) return;
            vi = v->second;
        }
        T &ending = t_[static_cast<size_t>(view_[static_cast<size_t>(vi)])];
        auto is_eos = [&](int v) {
            if (v < 0 || static_cast<size_t>(v) >= view_.size()) return false;
            const K k = t_[static_cast<size_t>(view_[static_cast<size_t>(v)])].kind;
            return k == K::Semicolon || k == K::Comma || k == K::Newline;
        };
        std::vector<int> term;
        int first_newline = -1;
        int v = vi + 1;
        while (is_eos(v)) {
            term.push_back(view_[static_cast<size_t>(v)]);
            v++;
            if (t_[static_cast<size_t>(term.back())].kind == K::Newline) {
                first_newline = static_cast<int>(term.size()) - 1;
                break;
            }
        }
        while (is_eos(v)) {
            term.push_back(view_[static_cast<size_t>(v)]);
            v++;
        }
        for (int ti : term) {
            T &t = t_[static_cast<size_t>(ti)];
            t.fix.statement_terminator = true;
            t.linked = true;
            t.level = e.level;
            t.causes = e.causes;
        }
        if (term.empty()) {
            if (static_cast<size_t>(v) >= view_.size()) return;  // EOF
            // Nothing between this and the next statement (`if x y = 1, end`).
            ending.fix.flag_continuations = true;
            ending.fix.add_newline = true;
            return;
        }
        T &t0 = t_[static_cast<size_t>(term[0])];
        if (e.semi) {
            if (t0.kind == K::Semicolon) {
            } else if (t0.kind == K::Comma) {
                if (opt_.add_semicolons) t0.fix.change_to_semicolon = true;
                else t0.fix.del = true;
            } else if (opt_.add_semicolons) {
                ending.fix.add_semicolon_after = true;
            }
            if (first_newline < 0) {
                if (t0.fix.del) ending.fix.add_newline = true;
                else t0.fix.add_newline = true;
            }
        } else if (t0.kind != K::Newline) {
            t0.fix.del = true;
            if (first_newline < 0) ending.fix.add_newline = true;
        }
        for (size_t i = 1; i < term.size(); i++) {
            T &t = t_[static_cast<size_t>(term[i])];
            if (t.kind != K::Newline) t.fix.del = true;
        }
    }

    // m_parser.py's parse_row_list / parse_row: the separators a matrix or a cell row does not need.
    void Rows() {
        for (size_t vi = 0; vi < view_.size(); vi++) {
            const K k = t_[static_cast<size_t>(view_[vi])].kind;
            if (k == K::MBra || k == K::CBra) RowList(vi + 1);
        }
    }
    K VK(size_t v) const { return v < view_.size() ? t_[static_cast<size_t>(view_[v])].kind : K::Newline; }
    T &VT(size_t v) { return t_[static_cast<size_t>(view_[v])]; }
    bool AtEnd(size_t v) const { return v >= view_.size(); }
    // Skips one element (an expression): up to a top-level , ; NEWLINE or closing bracket.
    size_t SkipElement(size_t v) const {
        int depth = 0;
        size_t last = v;
        for (; v < view_.size(); v++) {
            const K k = VK(v);
            if (depth == 0 && (k == K::Comma || k == K::Semicolon || k == K::Newline || IsKet(k) || k == K::AKet))
                break;
            if (k == K::Bra || k == K::MBra || k == K::CBra || k == K::ABra) depth++;
            if (IsKet(k) || k == K::AKet) depth--;
            last = v;
        }
        (void)last;
        return v;
    }
    void RowList(size_t v) {
        while (!(AtEnd(v) || VK(v) == K::CKet || VK(v) == K::MKet)) {
            // parse_row
            bool empty = true;
            if (!(VK(v) == K::Newline || VK(v) == K::Semicolon || VK(v) == K::CKet || VK(v) == K::MKet)) {
                bool done = false;
                if (VK(v) == K::Comma) {
                    VT(v).fix.spurious = true;
                    v++;
                    if (VK(v) == K::Newline || VK(v) == K::Semicolon || VK(v) == K::Comma || VK(v) == K::CKet ||
                        VK(v) == K::MKet)
                        done = true;
                }
                while (!done && !AtEnd(v)) {
                    const size_t e = SkipElement(v);
                    if (e == v) break;  // not an expression: give up on this bracket
                    empty = false;
                    size_t ct = e - 1;
                    v = e;
                    if (VK(v) == K::Comma) {
                        ct = v;
                        v++;
                    }
                    if (VK(v) == K::Newline || VK(v) == K::Semicolon || VK(v) == K::CKet || VK(v) == K::MKet ||
                        AtEnd(v)) {
                        VT(ct).fix.spurious = true;
                        break;
                    }
                }
            }
            // separators
            std::vector<size_t> seps;
            bool first_nl = false;
            size_t first_semi = static_cast<size_t>(-1);
            if (VK(v) == K::Newline || VK(v) == K::Semicolon) {
                while (!AtEnd(v) && (VK(v) == K::Newline || VK(v) == K::Semicolon)) {
                    if (VK(v) == K::Newline) {
                        first_nl = true;
                    } else {
                        if (first_semi == static_cast<size_t>(-1)) first_semi = v;
                        seps.push_back(v);
                    }
                    v++;
                }
            } else {
                break;
            }
            const bool all = first_nl || empty || AtEnd(v) || VK(v) == K::CKet || VK(v) == K::MKet;
            for (size_t s : seps)
                if (all || s != first_semi) VT(s).fix.spurious = true;
        }
    }

    // check_redundant_brackets: brackets around brackets, and around a whole expression
    // where MISS_HIT's parse_expression is used (conditions, ranges, arguments, ...).
    void Brackets() {
        std::vector<bool> drop(p_.nodes.size(), false);
        auto top = [&](int e) {
            if (e >= 0 && N(e).kind == Node::Paren) drop[static_cast<size_t>(e)] = true;
        };
        for (size_t i = 0; i < p_.nodes.size(); i++) {
            const AstNode &n = p_.nodes[i];
            switch (n.kind) {
            case Node::Paren:
                if (!n.kids.empty() && N(n.kids[0]).kind == Node::Paren) drop[static_cast<size_t>(n.kids[0])] = true;
                break;
            case Node::ExprStmt:
                if (!n.kids.empty()) top(n.kids[0]);
                break;
            case Node::Assign:
                if (n.kids.size() > 1 && N(n.kids[0]).kind == Node::Matrix) top(n.kids[1]);
                break;
            case Node::IfClause:
            case Node::While:
            case Node::Switch:
            case Node::Case:
                if (!n.kids.empty()) top(n.kids[0]);
                break;
            case Node::For:
                if (n.kids.size() > 1) top(n.kids[1]);
                if (n.kids.size() > 3) top(n.kids[3]);
                break;
            case Node::Index:
                for (size_t k = 1; k < n.kids.size(); k++) top(n.kids[k]);
                break;
            case Node::DynField:
                if (n.kids.size() > 1) top(n.kids[1]);
                break;
            default:
                break;
            }
        }
        for (size_t i = 0; i < drop.size(); i++) {
            if (!drop[i]) continue;
            const AstNode &n = p_.nodes[i];
            auto a = mh_of_.find(n.op >= 0 ? n.op : n.first);
            auto b = mh_of_.find(n.last);
            if (a == mh_of_.end() || b == mh_of_.end()) continue;
            if (t_[static_cast<size_t>(a->second)].kind != K::Bra || t_[static_cast<size_t>(b->second)].kind != K::Ket)
                continue;
            // Octave lets a bracketed expression run over lines with no `...`; without the
            // brackets those line breaks would end the statement. (MATLAB, MISS_HIT's
            // language, needs the `...`, so mh_style never meets this.)
            bool spans = false;
            for (int k = a->second + 1; k < b->second; k++)
                if (t_[static_cast<size_t>(k)].kind == K::Newline) spans = true;
            if (spans) continue;
            t_[static_cast<size_t>(a->second)].fix.del = true;
            t_[static_cast<size_t>(b->second)].fix.del = true;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// mh_style.py's stage_3_analysis: the token rules.

// Whether only comment lines stand between the `...` at `n` and a closing `]` or `}`: then
// the line breaks it would keep are row breaks before the end of the matrix anyway.
bool ClosesAfterComments(const std::vector<T> &tb, size_t n) {
    for (size_t i = n + 1; i < tb.size(); i++) {
        const K k = tb[i].kind;
        if (k == K::Comment || k == K::Newline) continue;
        return k == K::MKet || k == K::CKet;
    }
    return false;
}

bool KeywordWithWs(const std::string &w) {
    static const char *const kw[] = {"case", "catch", "classdef", "elseif", "for", "function", "global", "if",
                                     "parfor", "persistent", "switch", "while", "properties", "methods", "events"};
    for (const char *k : kw)
        if (w == k) return true;
    return false;
}

bool StartsAlpha(const std::string &s, size_t i) {
    return i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) != 0;
}

void Stage3(std::vector<T> &tb, int tab) {
    int stmt_start = -1;
    int current_indent = 0;
    bool have_enclosing = false;
    int enclosing_level = 0;
    std::vector<int> bstack;
    const size_t n_tok = tb.size();
    for (size_t n = 0; n < n_tok; n++) {
        T &token = tb[n];
        T *prev = n > 0 ? &tb[n - 1] : nullptr;
        T *next = n + 1 < n_tok ? &tb[n + 1] : nullptr;
        T *prev_in_line = nullptr, *next_in_line = nullptr;
        int ws_before = 0, ws_after = 0;
        if (prev && prev->line == token.line) {
            prev_in_line = prev;
            ws_before = token.cs - prev->ce - 1;
        }
        if (next && next->line == token.line && next->kind != K::Newline) {
            next_in_line = next;
            ws_after = next->cs - token.ce - 1;
        }
        if (token.fis) {
            if (stmt_start >= 0) {
                const T &s = tb[static_cast<size_t>(stmt_start)];
                if (s.kind == K::Keyword && IsEndKeyword(s.value)) {
                    have_enclosing = false;
                } else if (s.linked && s.causes) {
                    have_enclosing = true;
                    enclosing_level = s.level;
                }
            }
            stmt_start = static_cast<int>(n);
        }
        if (token.anon) continue;

        switch (token.kind) {
        case K::Comma:
        case K::Semicolon:
            token.fix.trim_before = true;
            token.fix.ws_after = true;
            if (token.fix.spurious) token.fix.del = true;
            break;
        case K::Colon:
            if (prev_in_line && prev_in_line->kind == K::Comma) {
            } else if (next_in_line && next_in_line->kind == K::Continuation) {
                if (prev_in_line && ws_before > 0) token.fix.trim_before = true;
            } else if ((prev_in_line && ws_before > 0) || (next_in_line && ws_after > 0)) {
                token.fix.trim_before = true;
                token.fix.trim_after = true;
            }
            break;
        case K::Assignment:
            token.fix.ws_before = true;
            token.fix.ws_after = true;
            break;
        case K::Bra:
        case K::ABra:
        case K::MBra:
            if (next_in_line && ws_after > 0 && next_in_line->kind != K::Continuation) token.fix.trim_after = true;
            break;
        case K::Ket:
        case K::AKet:
        case K::MKet:
            if (prev_in_line && ws_before > 0) token.fix.trim_before = true;
            break;
        case K::Keyword: {
            // (not before a `,` or `;`, which wants no space before it: `case, "x"`)
            if (KeywordWithWs(token.value) && next_in_line && ws_after == 0 &&
                next_in_line->kind != K::Comma && next_in_line->kind != K::Semicolon)
                token.fix.ws_after = true;
            if (token.value == "function") {
                size_t fstart = n;
                T *fstart_prev = prev;
                for (size_t i = n; i-- > 0;) {
                    if (tb[i].kind == K::Comment && tb[i].fil) {
                        fstart = i;
                        fstart_prev = i > 0 ? &tb[i - 1] : nullptr;
                    } else if (tb[i].kind == K::Newline &&
                               std::count(tb[i].raw.begin(), tb[i].raw.end(), '\n') == 1) {
                    } else {
                        break;
                    }
                }
                if (fstart_prev && !(fstart_prev->line + 1 < tb[fstart].line)) fstart_prev->fix.add_newline = true;
            }
            if (token.fn_end) {
                size_t true_end = n;
                T *true_end_nl = nullptr;
                T *true_end_next = nullptr;
                for (size_t i = n + 1; i < n_tok; i++) {
                    if (tb[i].kind == K::Newline) {
                        true_end_nl = &tb[i];
                        break;
                    } else if (tb[i].line == token.line) {
                        true_end = i;
                    } else {
                        break;
                    }
                }
                for (size_t i = true_end + 1; i < n_tok; i++) {
                    if (tb[i].kind != K::Newline) {
                        true_end_next = &tb[i];
                        break;
                    }
                }
                if (true_end_next && !(tb[true_end].line + 1 < true_end_next->line)) {
                    if (true_end_nl) true_end_nl->fix.add_newline = true;
                    else tb[true_end].fix.add_newline = true;
                }
            }
            break;
        }
        case K::Comment: {
            const std::string &r = token.raw;
            const char cchar = r[0];
            size_t body_at = 0;
            while (body_at < r.size() && r[body_at] == cchar) body_at++;
            const std::string body = r.substr(body_at);
            const std::string st = Strip(r);
            if (r.size() >= 3 && r[0] == '%' && r[1] == '#' && StartsAlpha(r, 2)) {
            } else if (r.compare(0, 2, "%!") == 0) {
            } else if (r.compare(0, 2, "%|") == 0) {
            } else if (token.block) {
            } else if (st == "%{" || st == "%}" || st == "#{" || st == "#}") {
            } else if (r.size() >= 3 && r[0] == '%' && r[1] == '#' && r[2] == ' ' &&
                       StartsAlpha(r, r.find_first_not_of(' ', 2))) {
                token.raw = "%#" + Strip(r.substr(2));
            } else if (r.size() >= 3 && r[0] == '%' && r[1] == ' ' && r.find_first_not_of(' ', 1) != std::string::npos &&
                       r[r.find_first_not_of(' ', 1)] == '#' && StartsAlpha(r, r.find_first_not_of(' ', 1) + 1)) {
                token.raw = "%#" + r.substr(r.find('#') + 1);
            } else if (!body.empty() && body[0] != ' ') {
                token.raw = std::string(body_at, cchar) + " " + body;
            }
            if (prev_in_line && ws_before == 0) token.fix.ws_before = true;
            break;
        }
        case K::Continuation:
            if (prev_in_line && ws_before == 0) token.fix.ws_before = true;
            // A blank line ends the statement anyway, so the `...` is useless. A comment line
            // does not end it in Octave (it does in MATLAB, MISS_HIT's language): there the
            // `...` only goes when the statement has already ended.
            if (next && (next->kind == K::Newline ||
                         (next->kind == K::Comment && ((prev && prev->fix.statement_terminator) ||
                                                       ClosesAfterComments(tb, n))))) {
                token.fix.replace_with_newline = true;
            } else if (prev && prev->fix.statement_terminator) {
                token.fix.del = true;
            }
            break;
        case K::Operator:
            if (token.fix.unary) {
                // Octave's postfix `++`/`--` sit against their operand like a transpose.
                if (prev_in_line && ws_before > 0 && token.fix.postfix) {
                    token.fix.trim_before = true;
                } else if (next_in_line && ws_after > 0 && !token.fix.postfix && next_in_line->kind != K::Continuation) {
                    token.fix.trim_after = true;
                }
            } else if (token.fix.binary) {
                if (token.value == ".^" || token.value == "^") {
                    if ((prev_in_line && ws_before > 0) || (next_in_line && ws_after > 0)) {
                        token.fix.trim_before = true;
                        // (a `...` keeps its space, or the next run would put it back)
                        if (!next_in_line || next_in_line->kind != K::Continuation) token.fix.trim_after = true;
                    }
                } else if ((prev_in_line && ws_before == 0) || (next_in_line && ws_after == 0)) {
                    token.fix.ws_before = true;
                    token.fix.ws_after = true;
                }
            }
            break;
        case K::Newline:
            if (n == 0) token.fix.del = true;
            break;
        default:
            break;
        }

        if (token.fix.flag_continuations && next_in_line && next_in_line->kind == K::Continuation) {
            token.fix.add_newline = false;
            next_in_line->fix.replace_with_newline = true;
        }

        if (token.kind != K::Newline && token.fil && !(token.block && token.value != "{")) {
            int offset = 0;
            if (token.fis) {
                if (token.linked) current_indent = token.level;
                else if (have_enclosing) current_indent = enclosing_level + 1;
                offset = 0;
            } else if (!bstack.empty()) {
                const T &b = tb[static_cast<size_t>(bstack.back())];
                const int start_cs = stmt_start >= 0 ? tb[static_cast<size_t>(stmt_start)].cs : 0;
                offset = (b.fix.correct_indent >= 0 ? b.fix.correct_indent : b.cs) - start_cs;
                if (!IsKet(token.kind)) offset += 1;
            } else {
                const int start_cs = stmt_start >= 0 ? tb[static_cast<size_t>(stmt_start)].cs : 0;
                offset = token.cs - start_cs;
                if (offset <= 0) offset = tab / 2;
            }
            token.fix.correct_indent = tab * current_indent + offset;
        }

        if (token.kind == K::MBra || token.kind == K::CBra || token.kind == K::Bra) {
            bstack.push_back(static_cast<int>(n));
        } else if (IsKet(token.kind)) {
            if (!bstack.empty()) bstack.pop_back();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Token_Buffer.replay

void FixupWs(T &t, bool before) {
    if (before) {
        if (t.kind == K::Bra || t.kind == K::CBra || t.kind == K::MBra) {
            t.fix.trim_after = true;
        } else if (t.kind == K::Semicolon || t.kind == K::Comma) {
            t.fix.ws_after = true;
            t.fix.maxgap_after = true;
        } else {
            t.fix.maxgap_after = true;
        }
    } else {
        if (IsKet(t.kind)) {
            t.fix.trim_before = true;
        } else if (t.kind == K::Semicolon || t.kind == K::Comma) {
            t.fix.trim_before = true;
            t.fix.maxgap_before = true;
        } else {
            t.fix.maxgap_before = true;
        }
    }
}

void Autofix(T &t) {
    if (t.fix.change_to_semicolon) {
        t.kind = K::Semicolon;
        t.raw = t.value = ";";
        t.fix.trim_before = true;
    } else if (t.fix.replace_with_newline) {
        t.fix.replace_with_newline = false;
        if (t.value.empty()) {
            t.kind = K::Newline;
            t.raw = t.value = "\n";
        } else {
            t.kind = K::Comment;
            size_t b = 0;
            while (b < t.value.size() && t.value[b] == '%') b++;
            t.value = Strip(t.value.substr(b));
            t.raw = "% " + t.value;
            t.fix.add_newline = true;
        }
    }
}

std::string Replay(std::vector<T> &tokens, int tab) {
    std::vector<T> nt;
    bool deleted = false, next_fil = false, next_fis = false;
    for (T &t : tokens) {
        if (t.fix.del) {
            deleted = true;
            next_fil = t.fil;
            next_fis = t.fis;
            if (!nt.empty()) {
                FixupWs(nt.back(), true);
                if (t.fix.add_semicolon_after) nt.back().fix.add_semicolon_after = true;
                if (t.fix.add_newline) nt.back().fix.add_newline = true;  // MISS_HIT drops this
            }
        } else if (t.anon) {
        } else {
            Autofix(t);
            nt.push_back(t);
            if (deleted) {
                T &b = nt.back();
                FixupWs(b, false);
                b.fil = next_fil;
                b.fis = next_fis;
                deleted = false;
                if (b.fis && b.linked) b.fix.correct_indent = b.level * tab;
            }
        }
    }
    // add newlines
    std::vector<T> withnl;
    bool newline_added = false;
    int shift = 0;
    const T *previous = nullptr;
    int block_indent = 0;
    for (T &t : nt) {
        t.line += shift;
        withnl.push_back(t);
        T &cur = withnl.back();
        if (cur.fis && cur.linked) block_indent = cur.level;
        if (newline_added) {
            newline_added = false;
            cur.fil = true;
            cur.fis = true;
            if (cur.linked) cur.fix.correct_indent = cur.level * tab;
            else if (previous && previous->linked && !previous->causes) cur.fix.correct_indent = previous->level * tab;
            else cur.fix.correct_indent = block_indent * tab;
        }
        if (cur.fix.add_newline) {
            if (cur.kind == K::Newline) {
                cur.raw += "\n";
                cur.value += "\n";
            } else {
                newline_added = true;
                T nl;
                nl.kind = K::Newline;
                nl.raw = nl.value = "\n";
                nl.line = cur.line;
                nl.cs = cur.cs;
                nl.ce = cur.ce;
                nl.anon = true;
                withnl.push_back(nl);
            }
            shift += 1;
        }
        previous = &t;
    }
    // strip duplicate newlines
    std::vector<T> fin;
    for (T &t : withnl) {
        if (!fin.empty() && fin.back().kind == K::Newline && t.kind == K::Newline) {
            if (t.raw.size() == 1 && fin.back().raw.size() == 1) {
                fin.back().raw += "\n";
                fin.back().value += "\n";
            }
            continue;
        }
        fin.push_back(t);
    }
    std::string rv;
    for (size_t n = 0; n < fin.size(); n++) {
        const T &t = fin[n];
        const T *next_in_line = n + 1 < fin.size() && fin[n + 1].line == t.line ? &fin[n + 1] : nullptr;
        if (t.fil) {
            const int ind = t.fix.correct_indent >= 0 ? t.fix.correct_indent : t.cs;
            rv.append(static_cast<size_t>(std::max(0, ind)), ' ');
        }
        if (t.kind == K::Newline) {
            int amount = std::min(2, static_cast<int>(std::count(t.raw.begin(), t.raw.end(), '\n')));
            if (n + 1 == fin.size()) amount = 0;
            rv.append(static_cast<size_t>(amount), '\n');
        } else if (t.kind == K::Continuation) {
            rv += Rstrip(t.raw) + "\n";
        } else {
            rv += Rstrip(t.raw);
        }
        if (t.fix.add_semicolon_after) rv += ";";
        if (next_in_line && next_in_line->kind != K::Newline) {
            int gap = next_in_line->cs - (t.ce + 1);
            if (t.fix.ws_after || next_in_line->fix.ws_before) gap = std::max(gap, 1);
            else if (t.fix.trim_after || next_in_line->fix.trim_before) gap = 0;
            if ((t.fix.maxgap_after || next_in_line->fix.maxgap_before) && next_in_line->kind != K::Comment)
                gap = std::min(gap, 1);
            if (gap > 0) rv.append(static_cast<size_t>(gap), ' ');
        }
    }
    rv += "\n";
    return rv;
}

// MISS_HIT reads the file with universal newlines, splits it into lines and
// expands tabs (to the next multiple of tab_width) before lexing.
std::string Prepare(std::string_view src, int tab) {
    std::string out;
    out.reserve(src.size() + 16);
    std::string line;
    bool any = false;
    auto flush = [&]() {
        out += line;
        out += '\n';
        line.clear();
    };
    for (size_t i = 0; i < src.size(); i++) {
        const char c = src[i];
        any = true;
        if (c == '\r') {
            if (i + 1 < src.size() && src[i + 1] == '\n') i++;
            flush();
            any = false;
        } else if (c == '\n') {
            flush();
            any = false;
        } else if (c == '\t') {
            line.append(static_cast<size_t>(tab - static_cast<int>(line.size()) % tab), ' ');
        } else {
            line += c;
        }
    }
    if (any) flush();
    return out;
}

// The program's shape, for the self-check: every node of the tree but
// brackets (which formatting may drop when redundant), with its tokens.
// Separators and whitespace are not in the tree; a matrix `[a -b]` that
// became `[a - b]` would be.
void Shape(const ParseResult &r, int node, std::vector<std::string> *out, int depth) {
    if (node < 0 || depth > octparse::kMaxDepth * 2) return;
    const AstNode &n = r.nodes[static_cast<size_t>(node)];
    if (n.kind == Node::Row && n.kids.empty()) return;  // `[1 2;]`: Octave drops empty rows
    if (n.kind != Node::Paren) {
        size_t kids = n.kids.size();
        if (n.kind == Node::Matrix || n.kind == Node::Cell)
            kids = static_cast<size_t>(std::count_if(n.kids.begin(), n.kids.end(), [&](int k) {
                return !r.nodes[static_cast<size_t>(k)].kids.empty();
            }));
        std::string s = std::to_string(static_cast<int>(n.kind));
        if (n.tok >= 0) s += " " + r.tokens[static_cast<size_t>(n.tok)].text;
        if (n.op >= 0) s += " " + r.tokens[static_cast<size_t>(n.op)].text;
        if (n.name >= 0) s += " n";
        s += " " + std::to_string(kids);
        out->push_back(s);
    }
    if (n.name >= 0) Shape(r, n.name, out, depth + 1);
    for (int k : n.kids) Shape(r, k, out, depth + 1);
}
std::vector<std::string> Shape(const ParseResult &r) {
    std::vector<std::string> out;
    Shape(r, r.root, &out, 0);
    return out;
}

Result FormatOnce(std::string_view src, const Options &opts) {
    Result res;
    res.text = std::string(src);
    const int tab = std::max(2, opts.indent);
    bool blank = true;
    for (char c : src)
        if (!std::isspace(static_cast<unsigned char>(c))) blank = false;
    if (blank) {
        res.ok = true;
        return res;
    }
    const std::string text = Prepare(src, tab);
    const ParseResult p = octparse::Parse(text);
    if (!p.ok()) {
        res.error = p.error;
        res.error_line = p.error_line;
        return res;
    }
    std::vector<T> toks;
    std::string err;
    Lexer lex(text, p);
    if (!lex.Run(&toks, &err)) {
        res.error = err;
        return res;
    }
    Analyzer(p, toks, opts).Run();
    Stage3(toks, tab);
    std::string out = Replay(toks, tab);
    // Self-check: the result must read back as the same program.
    const ParseResult q = octparse::Parse(out);
    if (!q.ok()) {
        res.error = "internal error: the formatted text does not parse (" + q.error + " near line " +
                    std::to_string(q.error_line) + ")";
        res.error_line = q.error_line;
        return res;
    }
    if (Shape(p) != Shape(q)) {
        res.error = "internal error: formatting changed the program's tokens";
        return res;
    }
    res.ok = true;
    res.text = std::move(out);
    return res;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result r = FormatOnce(src, opts);
    // MISS_HIT aligns continuation lines with the columns the line had
    // before its own spacing was fixed, so a second run can move them again:
    // run to a fixed point (it takes two passes, rarely three).
    for (int pass = 0; opts.converge && r.ok && pass < 4; pass++) {
        Result again = FormatOnce(r.text, opts);
        if (!again.ok || again.text == r.text) break;
        r = std::move(again);
    }
    return r;
}

}  // namespace octavefmt
