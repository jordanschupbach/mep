// mep's own Fish formatter: fish_indent's layout, rebuilt from its documented
// and observable rules (see fish_format.h; no fish code is used).
//
// The text is read with fish's own reader (fish_parse.h) in its forgiving
// mode -- errors are recovered from, comments are kept, unfinished blocks
// are allowed -- and printed back token by token:
//
//   * every line is indented 4 spaces per level: job lists, the conditions
//     of `if` / `while`, the `case` arms, a continued pipeline or `&&` / `||`
//     line, the line after an escaped newline, and command substitutions
//     (indented relative to where they open; one-line ones are left alone);
//     multi-line quoted strings keep their text;
//   * the "gaps" between tokens keep their comments and their newlines
//     (runs of blank lines become one, leading and trailing ones go),
//     semicolons there are dropped, and an escaped newline survives only
//     before an argument, a redirection, `&&` / `||` / `|`, or a command
//     that follows variable assignments;
//   * a statement terminator is a newline, except a `;` the input already
//     had in `if x; and y`, `while x; or y`, a one-line `{ a; b; }` and
//     `x; and y` on one line;
//   * a word whose unquoted text is only letters, digits, `_`, `-` and `/`
//     loses its quotes; redirections lose the space before their target;
//     `begin` and a multi-line `{` start a line of their own.
//
// The result is checked before it is returned: it must read back as the same
// words, operators and comments, or the input comes back unchanged with an
// error. `just format-compare fish` compares the output with fish_indent.
#include "fish_format.h"

#include <algorithm>

#include "fish_parse.h"

namespace fishfmt {

namespace {

namespace fp = fishparse;
using u32 = std::u32string;
using u32v = std::u32string_view;

constexpr int kMaxCmdsubDepth = 100;

// --- indentation --------------------------------------------------------------------------

fp::ParseFlags IndentFlags() {
    fp::ParseFlags f;
    f.continue_after_error = true;
    f.include_comments = true;
    f.accept_incomplete_tokens = true;
    f.leave_unterminated = true;
    return f;
}

std::vector<int> ComputeIndents(u32v src, int initial, int depth);

class IndentVisitor {
public:
    IndentVisitor(const fp::Ast &ast, u32v src, std::vector<int> &indents, int initial, int depth)
        : ast_(ast), src_(src), indents_(indents), last_indent_(initial - 1), indent_(initial - 1), depth_(depth) {}

    void Visit(int n) {
        const fp::Node &node = ast_[n];
        int inc = 0, dec = 0;
        switch (node.kind) {
        case fp::Kind::JobList: case fp::Kind::AndorJobList: inc = dec = 1; break;
        case fp::Kind::JobConjunction: {
            const fp::Kind pk = ast_[node.parent].kind;
            if (pk == fp::Kind::IfClause || pk == fp::Kind::WhileHeader) inc = dec = 1;
            break;
        }
        case fp::Kind::JobContinuation: case fp::Kind::JobConjunctionContinuation:
            if (HasNewline(ast_.Field(n, fp::Role::Newlines))) inc = dec = 1;
            break;
        case fp::Kind::CaseItemList: {
            const int sw = node.parent;
            const int end = ast_.Field(sw, fp::Role::End);
            inc = 1;
            dec = end >= 0 && ast_[end].sourced ? 1 : 0;
            break;
        }
        case fp::Kind::Token:
            if (ast_[node.parent].kind == fp::Kind::BeginHeader && node.tok == fp::PTok::End && node.sourced &&
                src_.substr(node.start, node.len) == U"\n")
                inc = dec = 1;
            break;
        default: break;
        }
        const bool leaf = node.IsLeaf();
        const size_t rs = leaf && node.sourced ? node.start : 0;
        const size_t rl = leaf && node.sourced ? node.len : 0;
        if (rl > 0 && leaf) {
            RecordContinuationsUntil(rs);
            Fill(last_leaf_end_, rs, last_indent_);
        }
        indent_ += inc;
        if (inc != 0) last_indent_ = indent_;
        if (leaf && rl != 0) {
            size_t lead = 0;
            while (lead < rs && src_[rs - 1 - lead] == ' ') lead++;
            Fill(rs - lead, rs, indent_);
            IndentLeaf(rs, rl);
            last_leaf_end_ = rs + rl;
            last_indent_ = indent_;
        }
        for (int k : node.kids) Visit(k);
        indent_ -= dec;
    }

    void Finish() {
        RecordContinuationsUntil(indents_.size());
        Fill(last_leaf_end_, indents_.size(), last_indent_);
    }

    bool unclosed() const { return unclosed_; }
    int last_indent() const { return last_indent_; }
    const std::vector<size_t> &continuations() const { return continuations_; }

private:
    void Fill(size_t a, size_t b, int v) {
        for (size_t i = a; i < b && i < indents_.size(); i++) indents_[i] = v;
    }

    bool HasNewline(int nls) const {
        if (nls < 0) return false;
        const fp::Node &n = ast_[nls];
        if (!n.sourced) return false;
        return src_.substr(n.start, n.len).find(U'\n') != u32v::npos;
    }

    void RecordContinuationsUntil(size_t offset) {
        if (offset <= last_leaf_end_) return;
        const u32v gap = src_.substr(last_leaf_end_, offset - last_leaf_end_);
        size_t esc = u32v::npos;
        for (size_t i = 0; i + 1 < gap.size(); i++)
            if (gap[i] == '\\' && gap[i + 1] == '\n') {
                esc = i;
                break;
            }
        if (esc == u32v::npos) return;
        if (gap.substr(0, esc).find(U'#') != u32v::npos) return;
        size_t nl = esc + 1;
        for (;;) {
            continuations_.push_back(last_leaf_end_ + nl);
            const size_t next = gap.find(U'\n', nl + 1);
            if (next == u32v::npos) break;
            nl = next;
        }
    }

    void IndentLeaf(size_t start, size_t len) {
        const u32v node_src = src_.substr(start, len);
        if (node_src.find(U'(') != u32v::npos && node_src.find(U'\n') == u32v::npos) {
            Fill(start, start + len, indent_);
            return;
        }
        size_t done = start;
        size_t cursor = 0;
        bool is_dq = false, was_dq = false;
        for (;;) {
            was_dq = is_dq;
            fp::Cmdsub cs;
            if (fp::LocateCmdsub(node_src, &cursor, true, &is_dq, nullptr, &cs) != 1) break;
            IndentStringPart(done, start + cs.CommandStart(), was_dq);
            const u32v contents = node_src.substr(cs.CommandStart(), cs.CommandEnd() - cs.CommandStart());
            const std::vector<int> sub = depth_ < kMaxCmdsubDepth ? ComputeIndents(contents, indent_ + 1, depth_ + 1)
                                                                  : std::vector<int>(contents.size(), indent_ + 1);
            for (size_t i = 0; i < sub.size(); i++) indents_[start + cs.CommandStart() + i] = sub[i];
            done = start + cs.CommandEnd();
            if (cs.num_closing == 0) unclosed_ = true;
        }
        IndentStringPart(done, start + len, was_dq);
    }

    void IndentStringPart(size_t a, size_t b, bool is_dq) {
        size_t start = a;
        bool quoted = false;
        if (is_dq) {
            const size_t qe = fp::QuoteEnd(src_, a, '"');
            if (qe != u32v::npos) start = qe + 1;
            else quoted = true;
        }
        size_t done = start;
        if (!quoted && start <= b) {
            const u32v part = src_.substr(start, b - start);
            auto on_quote = [&](size_t offset) {
                if (!quoted) {
                    Fill(done, start + offset + 1, indent_);
                    done = start + offset + 1;
                } else {
                    size_t first_line = 0;
                    while (first_line < offset && src_[start + first_line] != '\n') first_line++;
                    Fill(start, start + first_line, indent_);
                    done = start + offset;
                }
                quoted = !quoted;
            };
            fp::Tokenizer tok(part, fp::kTokAcceptUnfinished, on_quote);
            fp::Tok t;
            while (tok.Next(&t)) {
            }
        }
        if (!quoted) Fill(done, b, indent_);
        else unclosed_ = true;
    }

    const fp::Ast &ast_;
    u32v src_;
    std::vector<int> &indents_;
    size_t last_leaf_end_ = 0;
    int last_indent_;
    bool unclosed_ = false;
    int indent_;
    std::vector<size_t> continuations_;
    int depth_;
};

std::vector<int> ComputeIndents(u32v src, int initial, int depth) {
    std::vector<int> indents(src.size(), 0);
    if (src.find(U'\n') == u32v::npos) return indents;
    const fp::Ast ast = fp::Parse(src, IndentFlags());
    IndentVisitor iv(ast, src, indents, initial, depth);
    iv.Visit(0);
    iv.Finish();
    size_t idx = src.size();
    int next_indent = iv.last_indent();
    while (idx != 0) {
        idx--;
        if (src[idx] == '\n') {
            const bool empty_middle = idx + 1 < src.size() && src[idx + 1] == '\n';
            const bool trailing_unclosed = idx == src.size() - 1 && iv.unclosed();
            if (!empty_middle && !trailing_unclosed) indents[idx] = next_indent;
        } else {
            next_indent = indents[idx];
        }
    }
    for (size_t i : iv.continuations()) {
        for (;;) {
            indents[i] += 1;
            i++;
            if (i == src.size() || src[i] == '\n') break;
        }
    }
    return indents;
}

// --- printing -----------------------------------------------------------------------------

struct GapFlags {
    bool allow_escaped_newlines = false;
    bool skip_space = false;
};

struct SRange {
    size_t start = 0, len = 0;
    size_t End() const { return start + len; }
};

bool GoodChar(char32_t c) { return fp::ValidVarNameChar(c) || c == '-' || c == '/'; }

class Printer {
public:
    Printer(u32v src, const fp::Ast &ast, int indent_width)
        : src_(src), ast_(ast), width_(static_cast<size_t>(indent_width)) {
        indents_ = ComputeIndents(src, 0, 0);
        ComputeGaps();
        ComputeMultiLineBraces();
        ComputePreferredSemis();
    }

    u32 Run() {
        out_.clear();
        Traverse(0);
        EmitGapTextBefore(SRange{src_.size(), 0}, GapFlags{});
        while (!out_.empty() && AtLineStart()) out_.pop_back();
        out_.push_back('\n');
        return out_;
    }

private:
    // fish's source_range(): the span of a node's sourced leaves, or (0, 0) if any is unsourced.
    SRange NodeRange(int n) const {
        fp::Range r;
        if (!ast_.RangeOf(n, &r)) return SRange{};
        return SRange{r.start, r.len};
    }
    u32v Sub(SRange r) const { return src_.substr(r.start, r.len); }
    size_t Indent(size_t i) const {
        if (indents_.empty()) return 0;
        const int v = indents_[std::min(i, indents_.size() - 1)];
        return v > 0 ? static_cast<size_t>(v) : 0;
    }

    void Leaves(int n, std::vector<SRange> *out) const {
        const fp::Node &node = ast_[n];
        if (node.IsLeaf()) {
            if (node.sourced && node.len > 0) out->push_back(SRange{node.start, node.len});
            return;
        }
        for (int k : node.kids) Leaves(k, out);
    }

    void ComputeGaps() {
        std::vector<SRange> toks;
        Leaves(0, &toks);
        toks.push_back(SRange{src_.size(), 0});
        size_t prev_end = 0;
        for (const SRange &t : toks) {
            if (t.start >= prev_end) gaps_.push_back(SRange{prev_end, t.start - prev_end});
            prev_end = t.start + t.len;
        }
    }

    void ComputeMultiLineBraces() {
        for (size_t n = 0; n < ast_.nodes.size(); n++) {
            if (ast_.nodes[n].kind != fp::Kind::BraceStatement) continue;
            const SRange r = NodeRange(static_cast<int>(n));
            if (Sub(r).find(U'\n') != u32v::npos) multi_brace_.push_back(r.start);
        }
        std::sort(multi_brace_.begin(), multi_brace_.end());
    }

    bool IsSemi(int semi_nl) const {
        if (semi_nl < 0 || !ast_[semi_nl].sourced) return false;
        return src_.substr(ast_[semi_nl].start, ast_[semi_nl].len) == U";";
    }
    void MarkSemi(int semi_nl) {
        if (IsSemi(semi_nl)) preferred_semis_.push_back(ast_[semi_nl].start);
    }

    void ComputePreferredSemis() {
        for (size_t i = 0; i < ast_.nodes.size(); i++) {
            const int n = static_cast<int>(i);
            const fp::Kind k = ast_[n].kind;
            if (k != fp::Kind::IfClause && k != fp::Kind::WhileHeader) continue;
            const int cond = ast_.Field(n, fp::Role::Condition);
            const int andors = ast_.Field(n, fp::Role::AndorTail);
            const auto &items = ast_[andors].kids;
            if (items.empty()) continue;
            MarkSemi(ast_.Field(cond, fp::Role::SemiNl));
            for (size_t j = 0; j + 1 < items.size(); j++)
                MarkSemi(ast_.Field(ast_.Field(items[j], fp::Role::Job), fp::Role::SemiNl));
        }
        for (size_t i = 0; i < ast_.nodes.size(); i++) {
            const int n = static_cast<int>(i);
            if (ast_[n].kind != fp::Kind::BraceStatement) continue;
            if (std::binary_search(multi_brace_.begin(), multi_brace_.end(), NodeRange(n).start)) continue;
            for (int job : ast_[ast_.Field(n, fp::Role::Jobs)].kids) MarkSemi(ast_.Field(job, fp::Role::SemiNl));
        }
        for (size_t i = 0; i < ast_.nodes.size(); i++) {
            const int n = static_cast<int>(i);
            if (ast_[n].kind != fp::Kind::JobList) continue;
            int prev = -1;
            for (int job : ast_[n].kids) {
                const int p = prev;
                prev = ast_.Field(job, fp::Role::SemiNl);
                const int deco = ast_.Field(job, fp::Role::Decorator);
                if (deco < 0 || p < 0 || !IsSemi(p)) continue;
                const size_t ps = ast_[p].start;
                const SRange dr = NodeRange(deco);
                if (dr.End() < ps) continue;
                if (src_.substr(ps, dr.End() - ps).find(U'\n') == u32v::npos) preferred_semis_.push_back(ps);
            }
        }
        std::sort(preferred_semis_.begin(), preferred_semis_.end());
    }

    GapFlags FlagsBefore(int n) const {
        GapFlags f;
        const fp::Node &node = ast_[n];
        switch (node.kind) {
        case fp::Kind::Argument: case fp::Kind::Redirection: case fp::Kind::VariableAssignment:
            f.allow_escaped_newlines = true;
            break;
        case fp::Kind::Token:
            if (node.tok == fp::PTok::AndAnd || node.tok == fp::PTok::OrOr || node.tok == fp::PTok::Pipe) {
                f.allow_escaped_newlines = true;
            } else if (node.tok == fp::PTok::String) {
                int p = node.parent;
                if (ast_[p].kind != fp::Kind::DecoratedStatement) return f;
                p = ast_[ast_[p].parent].parent;  // past the Statement
                if (p < 0) return f;
                const fp::Kind pk = ast_[p].kind;
                if (pk == fp::Kind::JobPipeline || pk == fp::Kind::JobContinuation || pk == fp::Kind::NotStatement) {
                    const int vars = ast_.Field(p, fp::Role::Variables);
                    if (vars >= 0 && !ast_[vars].kids.empty()) f.allow_escaped_newlines = true;
                }
            }
            break;
        default: break;
        }
        return f;
    }

    bool AtLineStart() const { return out_.empty() || out_.back() == '\n'; }

    size_t BackslashesBefore(u32v text, size_t idx) const {
        size_t n = 0;
        idx = std::min(idx, text.size());
        while (idx > 0 && text[idx - 1] == '\\') idx--, n++;
        return n;
    }

    bool HasPrecedingSpace() const {
        long idx = static_cast<long>(out_.size()) - 1;
        while (idx >= 0 && out_[static_cast<size_t>(idx)] == '\n') {
            // (fish counts these backslashes in the source text, at the output's index)
            const size_t bs = BackslashesBefore(src_, static_cast<size_t>(idx));
            if (bs % 2 == 0) return false;
            idx -= 1 + static_cast<long>(bs);
        }
        if (idx < 0) return false;
        const size_t i = static_cast<size_t>(idx);
        return out_[i] == ' ' && BackslashesBefore(out_, i) % 2 == 0;
    }

    void EmitSpaceOrIndent(GapFlags flags) {
        if (AtLineStart()) {
            out_.append(width_ * current_indent_, ' ');
        } else if (!flags.skip_space && !HasPrecedingSpace()) {
            out_.push_back(' ');
        }
    }

    bool EmitGapText(SRange range, GapFlags flags) {
        const u32v gap = Sub(range);
        if (gap.find_first_not_of(U' ') == u32v::npos) return false;
        const size_t esc = gap.find(U"\\\n");
        if (esc != u32v::npos) {
            const size_t comment = gap.find(U'#');
            if (flags.allow_escaped_newlines || (comment != u32v::npos && esc < comment)) {
                if (!AtLineStart() && !HasPrecedingSpace()) out_.push_back(' ');
                out_ += U"\\\n";
                current_indent_ = Indent(range.start + esc + 1);
                EmitSpaceOrIndent(GapFlags{});
            }
        }
        bool needs_nl = false;
        fp::Tokenizer tok(gap, fp::kTokShowComments | fp::kTokShowBlankLines);
        fp::Tok t;
        while (tok.Next(&t)) {
            const u32v text = gap.substr(t.offset, t.length);
            if (needs_nl) {
                out_.push_back('\n');
                needs_nl = false;
                mask_newline_ = false;
                if (text == U"\n") continue;
            } else if (mask_newline_) {
                if (t.type == fp::TokType::End) continue;
                mask_newline_ = false;
            }
            if (t.type == fp::TokType::Comment) {
                EmitSpaceOrIndent(GapFlags{});
                out_ += text;
                needs_nl = true;
            } else if (t.type == fp::TokType::End) {
                if (text == U"\n") {
                    out_.push_back('\n');
                    mask_newline_ = true;
                }
            } else {
                EmitSpaceOrIndent(GapFlags{});
                out_ += text;
            }
        }
        if (needs_nl) out_.push_back('\n');
        return needs_nl;
    }

    SRange GapTextTo(size_t end) const {
        const auto it = std::lower_bound(gaps_.begin(), gaps_.end(), end,
                                         [](const SRange &r, size_t e) { return r.End() < e; });
        if (it != gaps_.end() && it->End() == end) return *it;
        return SRange{};
    }

    bool RangeContainedError(SRange r) const {
        const auto &errs = ast_.errors;
        for (size_t i = 1; i < errs.size(); i++)
            if (!(errs[i - 1].End() <= errs[i].start)) return false;
        for (const fp::Range &e : errs)
            if (!(e.End() < r.start)) return true;
        return false;
    }

    bool EmitGapTextBefore(SRange r, GapFlags flags) {
        bool added = false;
        const SRange range = GapTextTo(r.start);
        if (range.len > 0) {
            if (range.start < indents_.size()) current_indent_ = Indent(range.start);
            if (RangeContainedError(range)) out_ += Sub(range);
            else added = EmitGapText(range, flags);
        }
        mask_newline_ = false;
        return added;
    }

    u32 CleanText(u32v input) const {
        u32 un;
        if (!fp::Unescape(input, true, false, &un, true)) return u32(input);
        un.erase(std::remove(un.begin(), un.end(), fp::kInternalSeparator), un.end());
        if (un.empty()) return u32(input);
        for (char32_t c : un)
            if (!GoodChar(c)) return u32(input);
        return un;
    }

    void EmitText(SRange r, GapFlags flags) {
        EmitGapTextBefore(r, flags);
        current_indent_ = Indent(r.start);
        if (r.len > 0) {
            EmitSpaceOrIndent(flags);
            out_ += CleanText(Sub(r));
        }
    }

    void EmitNodeText(int n) {
        SRange r = NodeRange(n);
        const u32v s = Sub(r);
        const bool esc_nl = s.size() >= 2 && s[s.size() - 2] == '\\' && s.back() == '\n';
        if (esc_nl) r.len -= 2;
        EmitText(r, FlagsBefore(n));
        if (esc_nl) {
            out_ += U" \\\n";
            current_indent_ += 1;
            EmitSpaceOrIndent(GapFlags{});
            current_indent_ -= 1;
        }
    }

    void VisitSemiNl(int n) {
        const SRange r = NodeRange(n);
        const bool prefer_semi =
            r.len > 0 && std::binary_search(preferred_semis_.begin(), preferred_semis_.end(), r.start);
        EmitGapTextBefore(r, FlagsBefore(n));
        if (!AtLineStart()) out_.push_back(prefer_semi ? ';' : '\n');
    }

    bool IsMultiLineBrace(int n) const {
        const int p = ast_[n].parent;
        if (ast_[p].kind != fp::Kind::BraceStatement) return false;
        return std::binary_search(multi_brace_.begin(), multi_brace_.end(), NodeRange(p).start);
    }

    bool BraceIsContinuation(int n) const {
        const int brace = ast_[n].parent;
        if (ast_[brace].kind != fp::Kind::BraceStatement) return false;
        const int st = ast_[brace].parent;
        if (st < 0 || ast_[st].kind != fp::Kind::Statement) return false;
        const int parent = ast_[st].parent;
        if (ast_[parent].kind == fp::Kind::NotStatement) return true;
        if (ast_[parent].kind != fp::Kind::JobPipeline) return false;
        if (ast_.Field(parent, fp::Role::Time) >= 0) return true;
        const int conj = ast_[parent].parent;
        if (ast_[conj].kind != fp::Kind::JobConjunction) return false;
        if (ast_.Field(conj, fp::Role::Decorator) >= 0) return true;
        const fp::Kind ck = ast_[ast_[conj].parent].kind;
        return ck == fp::Kind::IfClause || ck == fp::Kind::WhileHeader;
    }

    void VisitLeftBrace(int n) {
        const SRange r = NodeRange(n);
        const GapFlags flags = FlagsBefore(n);
        EmitGapTextBefore(r, flags);
        if (IsMultiLineBrace(n) && !AtLineStart() && !BraceIsContinuation(n)) out_.push_back('\n');
        current_indent_ = Indent(r.start);
        EmitSpaceOrIndent(flags);
        out_.push_back('{');
    }

    void VisitRightBrace(int n) {
        const SRange r = NodeRange(n);
        const GapFlags flags = FlagsBefore(n);
        EmitGapTextBefore(r, flags);
        if (IsMultiLineBrace(n)) {
            current_indent_ = Indent(r.start);
            if (!AtLineStart()) out_.push_back('\n');
            EmitSpaceOrIndent(flags);
            out_.push_back('}');
        } else {
            EmitNodeText(n);
        }
    }

    void VisitRedirection(int n) {
        const int oper = ast_.Field(n, fp::Role::Oper);
        if (oper < 0 || !ast_[oper].sourced) return;
        EmitText(SRange{ast_[oper].start, ast_[oper].len}, GapFlags{});
        const int target = ast_.Field(n, fp::Role::Target);
        if (target < 0 || !ast_[target].sourced) return;
        GapFlags f;
        f.skip_space = true;
        EmitText(SRange{ast_[target].start, ast_[target].len}, f);
    }

    void VisitMaybeNewlines(int n) {
        const fp::Node &node = ast_[n];
        if (!node.sourced || node.len == 0) return;
        const SRange r{node.start, node.len};
        const GapFlags flags = FlagsBefore(n);
        current_indent_ = Indent(r.start);
        const bool added = EmitGapTextBefore(r, flags);
        SRange g = r;
        if (added && g.len > 0 && src_[g.start] == '\n') g.start += 1;
        if (g.End() > src_.size()) g.len = src_.size() - std::min(g.start, src_.size());
        EmitGapText(g, flags);
    }

    void VisitBeginHeader(int n) {
        EmitNodeText(ast_.Field(n, fp::Role::Kw));
        const int semi = ast_.Field(n, fp::Role::SemiNl);
        if (semi >= 0) VisitSemiNl(semi);
        if (!AtLineStart()) out_.push_back('\n');
    }

    void Traverse(int n) {
        const fp::Node &node = ast_[n];
        switch (node.kind) {
        case fp::Kind::Keyword: EmitNodeText(n); return;
        case fp::Kind::Token:
            if (node.tok == fp::PTok::End) VisitSemiNl(n);
            else if (node.tok == fp::PTok::LeftBrace) VisitLeftBrace(n);
            else if (node.tok == fp::PTok::RightBrace) VisitRightBrace(n);
            else EmitNodeText(n);
            return;
        case fp::Kind::Argument: case fp::Kind::VariableAssignment: EmitNodeText(n); return;
        case fp::Kind::Redirection: VisitRedirection(n); return;
        case fp::Kind::MaybeNewlines: VisitMaybeNewlines(n); return;
        case fp::Kind::BeginHeader: VisitBeginHeader(n); return;
        default: break;
        }
        for (int k : node.kids) Traverse(k);
    }

    u32v src_;
    const fp::Ast &ast_;
    size_t width_;
    std::vector<int> indents_;
    u32 out_;
    size_t current_indent_ = 0;
    bool mask_newline_ = true;  // leading blank lines go
    std::vector<SRange> gaps_;
    std::vector<size_t> multi_brace_;
    std::vector<size_t> preferred_semis_;
};

// What the text says, for the self-check: words (unquoted), operators and comments.
std::vector<u32> Meaning(u32v src) {
    std::vector<u32> out;
    fp::Tokenizer tok(src, fp::kTokShowComments | fp::kTokContinueAfterError);
    fp::Tok t;
    while (tok.Next(&t)) {
        if (t.type == fp::TokType::End) continue;
        const u32v text = src.substr(t.offset, t.length);
        u32 m(1, static_cast<char32_t>('A' + static_cast<int>(t.type)));
        if (t.type == fp::TokType::String) {
            u32 un;
            if (fp::Unescape(text, false, true, &un)) m += un;
            else m += text;
        } else {
            m += text;
        }
        out.push_back(m);
    }
    return out;
}

int LineOf(u32v s, size_t off) {
    return 1 + static_cast<int>(std::count(s.begin(), s.begin() + static_cast<long>(std::min(off, s.size())), U'\n'));
}

}  // namespace

Result Format(std::string_view text, const Options &opts) {
    Result r;
    r.text = std::string(text);
    const fp::Source src = fp::Source::FromUtf8(text);
    fp::ParseFlags flags;
    flags.continue_after_error = true;
    flags.include_comments = true;
    flags.leave_unterminated = true;
    const fp::Ast ast = fp::Parse(src.text, flags);
    if (ast.nodes.size() == 1 && !ast.parse_errors.empty() && ast.parse_errors[0].start == 0 &&
        ast.parse_errors[0].text.find("too deep") != std::string::npos) {
        r.error = ast.parse_errors[0].text;
        return r;
    }
    const u32 out = Printer(src.text, ast, opts.indent > 0 ? opts.indent : 4).Run();
    const std::vector<u32> before = Meaning(src.text), after = Meaning(out);
    if (before != after) {
        size_t i = 0;
        while (i < before.size() && i < after.size() && before[i] == after[i]) i++;
        // Locate the first word that would change, for the message.
        fp::Tokenizer tok(src.text, fp::kTokShowComments | fp::kTokContinueAfterError);
        fp::Tok t;
        size_t k = 0;
        int line = 0;
        while (tok.Next(&t)) {
            if (t.type == fp::TokType::End) continue;
            if (k++ == i) {
                line = LineOf(src.text, t.offset);
                break;
            }
        }
        r.error = "formatting would change the meaning of the code here; left as it is";
        r.error_line = line;
        return r;
    }
    r.ok = true;
    r.text = fp::ToUtf8(out);
    return r;
}

}  // namespace fishfmt
