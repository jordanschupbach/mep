// go/printer port: see go_printer.h. printer.go's machinery (whitespace
// buffer, comment interspersing, trimmer, tabwriter hand-off) is in this
// file; nodes.go is go_printer_nodes.cpp. Copyright 2009 The Go Authors.
// All rights reserved. BSD-style license; the full notice is in
// go_parse.cpp.

#include "go_printer_impl.h"

#include <algorithm>

#include "go_doc_comment.h"
#include "go_printer_internal.h"
#include "go_tabwriter.h"

namespace goprint {

using namespace goast;

// --- LineTable -----------------------------------------------------------------

Position LineTable::PositionFor(Pos p) const {
    if (p == NoPos) return {};
    const int offset = p - 1;
    if (offset < 0 || offset > size_) return {};
    auto it = std::upper_bound(lines_.begin(), lines_.end(), offset);
    const int line = static_cast<int>(it - lines_.begin());
    if (line == 0) return {offset, 1, offset + 1};
    return {offset, line, offset - lines_[static_cast<size_t>(line - 1)] + 1};
}

Pos LineTable::LineStart(int line) const {
    if (line < 1 || line > LineCount()) return NoPos;
    return lines_[static_cast<size_t>(line - 1)] + 1;
}

void LineTable::MergeLine(int line) {
    if (line < 1 || line >= LineCount()) return;  // go panics
    lines_.erase(lines_.begin() + line);
}

// --- printer.go -------------------------------------------------------------------

namespace {

bool HasPrefix(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

bool IsDirective(std::string_view c) {
    if (HasPrefix(c, "line ") || HasPrefix(c, "extern ") || HasPrefix(c, "export ")) return true;
    size_t colon = c.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= c.size()) return false;
    for (size_t i = 0; i <= colon + 1; i++) {
        if (i == colon) continue;
        char b = c[i];
        if (!(('a' <= b && b <= 'z') || ('0' <= b && b <= '9'))) return false;
    }
    return true;
}

bool AllStars(std::string_view text) {
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            size_t j = i + 1;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) j++;
            if (j < text.size() && text[j] != '*') return false;
        }
    }
    return true;
}

bool IsBlankStr(std::string_view s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) > ' ') return false;
    return true;
}

std::string CommonPrefixC(std::string_view a, std::string_view b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i] && (static_cast<unsigned char>(a[i]) <= ' ' || a[i] == '*'))
        i++;
    return std::string(a.substr(0, i));
}

std::string_view TrimRight(std::string_view s) {
    while (!s.empty()) {
        int w = 0;
        uint32_t r = DecodeLastRune(s, &w);
        if (r == RuneError && w == 1) break;
        if (!UnicodeIsSpace(r)) break;
        s.remove_suffix(static_cast<size_t>(w));
    }
    return s;
}

void StripCommonPrefix(std::vector<std::string> &lines) {
    if (lines.size() <= 1) return;
    std::string prefix;
    bool prefix_set = false;
    if (lines.size() > 2) {
        for (size_t i = 1; i + 1 < lines.size(); i++) {
            if (IsBlankStr(lines[i])) {
                lines[i].clear();
            } else {
                if (!prefix_set) {
                    prefix = lines[i];
                    prefix_set = true;
                }
                prefix = CommonPrefixC(prefix, lines[i]);
            }
        }
    }
    if (!prefix_set) {
        const std::string &line = lines.back();
        prefix = CommonPrefixC(line, line);
    }
    bool line_of_stars = false;
    if (size_t star = prefix.find('*'); star != std::string::npos) {
        prefix.resize(star);
        if (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
        line_of_stars = true;
    } else {
        const std::string &first = lines[0];
        if (IsBlankStr(std::string_view(first).substr(2))) {
            size_t i = prefix.size();
            for (int n = 0; n < 3 && i > 0 && prefix[i - 1] == ' '; n++) i--;
            if (i == prefix.size() && i > 0 && prefix[i - 1] == '\t') i--;
            prefix.resize(i);
        } else {
            std::string suffix(first.size(), '\0');
            size_t n = 2;
            while (n < first.size() && static_cast<unsigned char>(first[n]) <= ' ') {
                suffix[n] = first[n];
                n++;
            }
            if (n > 2 && suffix[2] == '\t') {
                suffix = suffix.substr(2, n - 2);
            } else {
                suffix[0] = ' ';
                suffix[1] = ' ';
                suffix.resize(n);
            }
            if (prefix.size() >= suffix.size() && prefix.compare(prefix.size() - suffix.size(), suffix.size(), suffix) == 0)
                prefix.resize(prefix.size() - suffix.size());
        }
    }
    std::string &last = lines.back();
    std::string closing = "*/";
    std::string_view before = std::string_view(last).substr(0, last.find(closing));
    if (IsBlankStr(before)) {
        if (line_of_stars) closing = " */";
        last = prefix + closing;
    } else {
        prefix = CommonPrefixC(prefix, last);
    }
    for (size_t i = 1; i < lines.size(); i++) {
        if (!lines[i].empty()) lines[i] = lines[i].substr(std::min(prefix.size(), lines[i].size()));
    }
}

int Nlimit(int n) { return std::min(n, kMaxNewlines); }

bool MayCombine(Tok prev, char next) {
    switch (prev) {
    case Tok::INT: return next == '.';
    case Tok::ADD: return next == '+';
    case Tok::SUB: return next == '-';
    case Tok::QUO: return next == '*';
    case Tok::LSS: return next == '-' || next == '<';
    case Tok::AND: return next == '&' || next == '^';
    default: return false;
    }
}

std::string Trim(std::string_view data) {
    enum { inSpace, inEscape, inText } state = inSpace;
    std::string out, space;
    out.reserve(data.size());
    size_t m = 0;
    for (size_t n = 0; n < data.size(); n++) {
        char b = data[n];
        if (b == '\v') b = '\t';
        switch (state) {
        case inSpace:
            switch (b) {
            case '\t': case ' ': space += b; break;
            case '\n': case '\f':
                space.clear();
                out += '\n';
                break;
            case gotab::Escape:
                out += space;
                state = inEscape;
                m = n + 1;
                break;
            default:
                out += space;
                state = inText;
                m = n;
                break;
            }
            break;
        case inEscape:
            if (b == gotab::Escape) {
                out.append(data.substr(m, n - m));
                state = inSpace;
                space.clear();
            }
            break;
        case inText:
            switch (b) {
            case '\t': case ' ':
                out.append(data.substr(m, n - m));
                state = inSpace;
                space.clear();
                space += b;
                break;
            case '\n': case '\f':
                out.append(data.substr(m, n - m));
                state = inSpace;
                space.clear();
                out += '\n';
                break;
            case gotab::Escape:
                out.append(data.substr(m, n - m));
                state = inEscape;
                m = n + 1;
                break;
            default: break;
            }
            break;
        }
    }
    if (state == inEscape || state == inText) out.append(data.substr(m));
    return out;
}

}  // namespace

std::vector<Comment *> Printer::FormatDocComment(const std::vector<Comment *> &list) {
    std::string kind, text;
    std::vector<Comment *> directives;
    if (list.size() == 1 && HasPrefix(list[0]->Text, "/*")) {
        kind = "/*";
        text = list[0]->Text;
        if (text.find('\n') == std::string::npos || AllStars(text)) return list;
        text = text.substr(2, text.size() - 4);
    } else if (HasPrefix(list[0]->Text, "//")) {
        kind = "//";
        for (Comment *c : list) {
            if (!HasPrefix(c->Text, "//")) return list;
            std::string_view after = std::string_view(c->Text).substr(2);
            if (IsDirective(after)) {
                directives.push_back(c);
                continue;
            }
            if (HasPrefix(after, " ")) after.remove_prefix(1);
            text += after;
            text += '\n';
        }
    } else {
        return list;
    }
    if (text.empty()) return list;
    text = godoc::ReformatComment(text);
    const Pos slash = list[0]->Slash;
    if (kind == "/*") {
        Comment *c = scratch_.New<Comment>();
        c->Slash = slash;
        c->Text = "/*\n" + text + "*/";
        return {c};
    }
    std::vector<Comment *> out;
    std::string_view rest = text;
    while (!rest.empty()) {
        size_t k = rest.find('\n');
        std::string_view line = rest.substr(0, k);
        rest = k == std::string_view::npos ? std::string_view() : rest.substr(k + 1);
        Comment *c = scratch_.New<Comment>();
        c->Slash = slash;
        if (line.empty()) c->Text = "//";
        else if (line[0] == '\t') c->Text = "//" + std::string(line);
        else c->Text = "// " + std::string(line);
        out.push_back(c);
    }
    if (!directives.empty()) {
        Comment *c = scratch_.New<Comment>();
        c->Slash = slash;
        c->Text = "//";
        out.push_back(c);
        for (Comment *d : directives) {
            Comment *cc = scratch_.New<Comment>();
            cc->Slash = slash;
            cc->Text = d->Text;
            out.push_back(cc);
        }
    }
    return out;
}

Printer::Printer(const Config &cfg, const LineTable &lt, std::unordered_map<const Node *, int> *node_sizes, int depth)
    : cfg_(cfg), lt_(lt), node_sizes_(node_sizes), depth_(depth) {
    pos_ = {0, 1, 1};
    out_ = {0, 1, 1};
    wsbuf_.reserve(kWsCap);
}

void Printer::Enter() {
    if (++depth_ > kMaxDepth) throw TooDeep{};
}

bool Printer::CommentsHaveNewline(const std::vector<Comment *> &list) {
    const int line = LineFor(list[0]->Slash);
    for (size_t i = 0; i < list.size(); i++) {
        if (i > 0 && LineFor(list[i]->Slash) != line) return true;
        const std::string &t = list[i]->Text;
        if (t.size() >= 2 && (t[1] == '/' || t.find('\n') != std::string::npos)) return true;
    }
    return false;
}

void Printer::NextComment() {
    while (ci_.cindex < comments_.size()) {
        CommentGroup *c = comments_[ci_.cindex];
        ci_.cindex++;
        if (!c->List.empty()) {
            ci_.comment = c;
            ci_.comment_offset = PosFor(c->List[0]->Slash).offset;
            ci_.comment_newline = CommentsHaveNewline(c->List);
            return;
        }
    }
    ci_.comment_offset = kInfinity;
}

bool Printer::CommentBefore(const Position &next) const {
    return ci_.comment_offset < next.offset && (!implied_semi_ || !ci_.comment_newline);
}

int Printer::CommentSizeBefore(const Position &next) {
    const CommentInfo saved = ci_;
    int size = 0;
    while (CommentBefore(next)) {
        for (Comment *c : ci_.comment->List) size += static_cast<int>(c->Text.size());
        NextComment();
    }
    ci_ = saved;
    return size;
}

void Printer::WriteIndent() {
    const int n = cfg_.indent + indent_;
    for (int i = 0; i < n; i++) output_ += '\t';
    pos_.offset += n;
    pos_.column += n;
    out_.column += n;
}

void Printer::WriteByte(char ch, int n) {
    if (end_alignment_) {
        switch (ch) {
        case '\t': case '\v': ch = ' '; break;
        case '\n': case '\f':
            ch = '\f';
            end_alignment_ = false;
            break;
        default: break;
        }
    }
    if (out_.column == 1) WriteIndent();
    if (n > 0) output_.append(static_cast<size_t>(n), ch);
    pos_.offset += n;
    if (ch == '\n' || ch == '\f') {
        pos_.line += n;
        out_.line += n;
        pos_.column = 1;
        out_.column = 1;
        return;
    }
    pos_.column += n;
    out_.column += n;
}

void Printer::WriteString(const Position &pos, std::string_view s, bool is_lit) {
    if (out_.column == 1) WriteIndent();
    if (pos.IsValid()) pos_ = pos;
    if (is_lit) output_ += gotab::Escape;
    output_ += s;
    int nlines = 0;
    size_t li = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\n' || s[i] == '\f') {
            nlines++;
            li = i;
            end_alignment_ = true;
        }
    }
    pos_.offset += static_cast<int>(s.size());
    if (nlines > 0) {
        pos_.line += nlines;
        out_.line += nlines;
        const int c = static_cast<int>(s.size() - li);
        pos_.column = c;
        out_.column = c;
    } else {
        pos_.column += static_cast<int>(s.size());
        out_.column += static_cast<int>(s.size());
    }
    if (is_lit) output_ += gotab::Escape;
    last_ = pos_;
}

void Printer::WriteCommentPrefix(const Position &pos, const Position &next, const Comment *prev, Tok tok) {
    if (output_.empty()) return;
    if (pos.line == last_.line && (prev == nullptr || prev->Text[1] != '/')) {
        bool has_sep = false;
        if (prev == nullptr) {
            size_t j = 0;
            for (size_t i = 0; i < wsbuf_.size(); i++) {
                const WS ch = wsbuf_[i];
                if (ch == WS::blank) {
                    wsbuf_[i] = WS::ignore;
                    continue;
                }
                if (ch == WS::vtab) {
                    has_sep = true;
                    continue;
                }
                if (ch == WS::indent) continue;
                j = i;
                break;
            }
            WriteWhitespace(j);
        }
        if (!has_sep) {
            char sep = '\t';
            if (pos.line == next.line) sep = ' ';
            WriteByte(sep, 1);
        }
    } else {
        bool dropped_linebreak = false;
        size_t j = 0;
        for (size_t i = 0; i < wsbuf_.size(); i++) {
            const WS ch = wsbuf_[i];
            if (ch == WS::blank || ch == WS::vtab) {
                wsbuf_[i] = WS::ignore;
                continue;
            }
            if (ch == WS::indent) continue;
            if (ch == WS::unindent) {
                if (i + 1 < wsbuf_.size() && wsbuf_[i + 1] == WS::unindent) continue;
                if (tok != Tok::RBRACE && pos.column == next.column) continue;
            } else if (ch == WS::newline || ch == WS::formfeed) {
                wsbuf_[i] = WS::ignore;
                dropped_linebreak = prev == nullptr;
            }
            j = i;
            break;
        }
        WriteWhitespace(j);
        int n = 0;
        if (pos.IsValid() && last_.IsValid()) {
            n = pos.line - last_.line;
            if (n < 0) n = 0;
        }
        if (indent_ == 0 && dropped_linebreak) n++;
        if (n == 0 && prev != nullptr && prev->Text[1] == '/') n = 1;
        if (n > 0) WriteByte('\f', Nlimit(n));
    }
}

void Printer::WriteComment(const Comment *comment) {
    const std::string &text = comment->Text;
    Position pos = PosFor(comment->Slash);
    const int saved_indent = indent_;
    bool restore = false;
    if (HasPrefix(text, "//line ") && (!pos.IsValid() || pos.column == 1)) {
        indent_ = 0;
        restore = true;
    }
    if (text[1] == '/') {
        if (IsGoBuild(text)) go_build_.push_back(static_cast<int>(output_.size()));
        else if (IsPlusBuild(text)) plus_build_.push_back(static_cast<int>(output_.size()));
        WriteString(pos, TrimRight(text), true);
        if (restore) indent_ = saved_indent;
        return;
    }
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (true) {
            size_t k = text.find('\n', start);
            lines.push_back(text.substr(start, k == std::string::npos ? std::string::npos : k - start));
            if (k == std::string::npos) break;
            start = k + 1;
        }
    }
    if (pos.IsValid() && pos.column == 1 && indent_ > 0) {
        for (size_t i = 1; i < lines.size(); i++) lines[i] = "   " + lines[i];
    }
    StripCommonPrefix(lines);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) {
            WriteByte('\f', 1);
            pos = pos_;
        }
        if (!lines[i].empty()) WriteString(pos, TrimRight(lines[i]), true);
    }
    if (restore) indent_ = saved_indent;
}

std::pair<bool, bool> Printer::WriteCommentSuffix(bool needs_linebreak) {
    bool wrote_newline = false, dropped_ff = false;
    for (WS &ch : wsbuf_) {
        switch (ch) {
        case WS::blank: case WS::vtab: ch = WS::ignore; break;
        case WS::newline: case WS::formfeed:
            if (needs_linebreak) {
                needs_linebreak = false;
                wrote_newline = true;
            } else {
                if (ch == WS::formfeed) dropped_ff = true;
                ch = WS::ignore;
            }
            break;
        default: break;
        }
    }
    WriteWhitespace(wsbuf_.size());
    if (needs_linebreak) {
        WriteByte('\n', 1);
        wrote_newline = true;
    }
    return {wrote_newline, dropped_ff};
}

bool Printer::ContainsLinebreak() const {
    for (WS ch : wsbuf_)
        if (ch == WS::newline || ch == WS::formfeed) return true;
    return false;
}

std::pair<bool, bool> Printer::IntersperseComments(const Position &next, Tok tok) {
    const Comment *last = nullptr;
    while (CommentBefore(next)) {
        std::vector<Comment *> list = ci_.comment->List;
        bool changed = false;
        if (last_tok_ != Tok::IMPORT && PosFor(ci_.comment->List[0]->Slash).column == 1 &&
            PosFor(NodeEnd(ci_.comment) + 1) == next) {
            list = FormatDocComment(list);
            changed = true;
            if (!ci_.comment->List.empty() && list.empty()) {
                WriteCommentPrefix(PosFor(ci_.comment->List[0]->Slash), next, last, tok);
                pos_ = next;
                last_ = next;
                NextComment();
                return WriteCommentSuffix(false);
            }
        }
        for (Comment *c : list) {
            WriteCommentPrefix(PosFor(c->Slash), next, last, tok);
            WriteComment(c);
            last = c;
        }
        if (!ci_.comment->List.empty() && changed) {
            last = ci_.comment->List.back();
            pos_ = PosFor(NodeEnd(last));
            last_ = pos_;
        }
        NextComment();
    }
    if (last != nullptr) {
        bool needs_linebreak = false;
        if ((mode_ & kNoExtraBlank) == 0 && last->Text[1] == '*' && LineFor(last->Slash) == next.line &&
            tok != Tok::COMMA && (tok != Tok::RPAREN || prev_open_ == Tok::LPAREN) &&
            (tok != Tok::RBRACK || prev_open_ == Tok::LBRACK)) {
            if (ContainsLinebreak() && (mode_ & kNoExtraLinebreak) == 0 && level_ == 0) {
                needs_linebreak = true;
            } else {
                WriteByte(' ', 1);
            }
        }
        if (last->Text[1] == '/' || tok == Tok::EOF_ || (tok == Tok::RBRACE && (mode_ & kNoExtraLinebreak) == 0))
            needs_linebreak = true;
        return WriteCommentSuffix(needs_linebreak);
    }
    return {false, false};
}

void Printer::WriteWhitespace(size_t n) {
    for (long i = 0; i < static_cast<long>(n); i++) {
        const size_t ui = static_cast<size_t>(i);
        const WS ch = wsbuf_[ui];
        switch (ch) {
        case WS::ignore: break;
        case WS::indent: indent_++; break;
        case WS::unindent:
            indent_--;
            if (indent_ < 0) indent_ = 0;
            break;
        case WS::newline: case WS::formfeed:
            if (ui + 1 < n && wsbuf_[ui + 1] == WS::unindent) {
                wsbuf_[ui] = WS::unindent;
                wsbuf_[ui + 1] = WS::formfeed;
                i--;
                continue;
            }
            WriteByte(static_cast<char>(ch), 1);
            break;
        default: WriteByte(static_cast<char>(ch), 1); break;
        }
    }
    wsbuf_.erase(wsbuf_.begin(), wsbuf_.begin() + static_cast<long>(n));
}

void Printer::RecordPrevOpen() {
    switch (last_tok_) {
    case Tok::ILLEGAL: break;
    case Tok::LPAREN: case Tok::LBRACK: prev_open_ = last_tok_; break;
    default: prev_open_ = Tok::ILLEGAL; break;
    }
}

void Printer::Item(PM m) {
    RecordPrevOpen();
    mode_ ^= static_cast<int>(m);
}

void Printer::Item(WS x) {
    RecordPrevOpen();
    if (x == WS::ignore) return;
    if (wsbuf_.size() == kWsCap) WriteWhitespace(wsbuf_.size());
    wsbuf_.push_back(x);
    if (x == WS::newline || x == WS::formfeed) implied_semi_ = false;
    last_tok_ = Tok::ILLEGAL;
}

void Printer::Item(const Ident *x) {
    RecordPrevOpen();
    last_tok_ = Tok::IDENT;
    Emit(x->Name, false, true);
}

void Printer::Item(const BasicLit *x) {
    RecordPrevOpen();
    last_tok_ = x->Kind;
    Emit(x->Value, true, true);
}

void Printer::Item(Tok x) {
    RecordPrevOpen();
    const char *s = TokString(x);
    if (MayCombine(last_tok_, s[0])) {
        wsbuf_.assign(1, WS::blank);
    }
    bool implied = false;
    switch (x) {
    case Tok::BREAK: case Tok::CONTINUE: case Tok::FALLTHROUGH: case Tok::RETURN: case Tok::INC: case Tok::DEC:
    case Tok::RPAREN: case Tok::RBRACK: case Tok::RBRACE:
        implied = true;
        break;
    default: break;
    }
    last_tok_ = x;
    Emit(s, false, implied);
}

void Printer::Item(std::string_view s) {
    RecordPrevOpen();
    last_tok_ = Tok::STRING;
    Emit(s, true, true);
}

void Printer::Emit(std::string_view data, bool is_lit, bool implied_semi) {
    const Position next = pos_;
    auto [wrote_newline, dropped_ff] = Flush(next, last_tok_);
    if (!implied_semi_) {
        int n = Nlimit(next.line - pos_.line);
        if (wrote_newline && n == kMaxNewlines) n = kMaxNewlines - 1;
        if (n > 0) {
            WriteByte(dropped_ff ? '\f' : '\n', n);
            implied_semi = false;
        }
    }
    if (line_ptr_) {
        *line_ptr_ = out_.line;
        line_ptr_ = nullptr;
    }
    WriteString(next, data, is_lit);
    implied_semi_ = implied_semi;
}

std::pair<bool, bool> Printer::Flush(const Position &next, Tok tok) {
    if (CommentBefore(next)) return IntersperseComments(next, tok);
    WriteWhitespace(wsbuf_.size());
    return {false, false};
}

void Printer::PrintNode(Node *node) {
    if (auto *f = As<File>(node)) comments_ = f->Comments;
    use_node_comments_ = comments_.empty();
    NextComment();
    Print(PM::none);
    if (IsExpr(node)) {
        Expr(node);
    } else if (IsStmt(node)) {
        if (node->kind == K::LabeledStmt) indent_ = 1;
        Stmt(node, false);
    } else if (node->kind == K::GenDecl || node->kind == K::FuncDecl || node->kind == K::BadDecl) {
        Decl(node);
    } else if (node->kind == K::ImportSpec || node->kind == K::ValueSpec || node->kind == K::TypeSpec) {
        Spec(node, 1, false);
    } else if (auto *f = As<File>(node)) {
        FileNode(f);
    }
}

std::string Printer::Finish() {
    implied_semi_ = false;
    Flush(Position{kInfinity, kInfinity, 0}, Tok::EOF_);
    FixGoBuildLines(output_, go_build_, plus_build_);
    if (cfg_.mode & RawFormat) return Trim(output_);
    int minwidth = cfg_.tabwidth;
    char padchar = '\t';
    if (cfg_.mode & UseSpaces) padchar = ' ';
    unsigned twmode = gotab::DiscardEmptyColumns;
    if (cfg_.mode & TabIndent) {
        minwidth = 0;
        twmode |= gotab::TabIndent;
    }
    std::string tw_out;
    gotab::Writer tw(&tw_out, minwidth, cfg_.tabwidth, 1, padchar, twmode);
    tw.Write(output_);
    tw.Flush();
    return Trim(tw_out);
}

int Printer::NodeSize(Node *n, int max_size) {
    if (auto it = node_sizes_->find(n); it != node_sizes_->end()) return it->second;
    int size = max_size + 1;
    (*node_sizes_)[n] = size;
    Config cfg;
    cfg.mode = RawFormat;
    cfg.tabwidth = 0;
    Printer sub(cfg, lt_, node_sizes_, depth_);
    sub.PrintNode(n);
    const std::string text = sub.Finish();
    const bool has_newline = text.find_first_of("\n\f") != std::string::npos;
    if (static_cast<int>(text.size()) <= max_size && !has_newline) {
        size = static_cast<int>(text.size());
        (*node_sizes_)[n] = size;
    }
    return size;
}

bool Fprint(const Config &cfg, const LineTable &lines, File *file, std::string *out, std::string *error) {
    std::unordered_map<const Node *, int> sizes;
    try {
        Printer p(cfg, lines, &sizes, 0);
        p.PrintNode(file);
        *out = p.Finish();
    } catch (const Printer::TooDeep &) {
        *error = "the program nests too deeply to format";
        return false;
    }
    return true;
}

}  // namespace goprint
