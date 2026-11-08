// mep's GAP formatter: see gap_format.h.
//
// GAP has no standard formatter; the reference is GAP's own function printer (`Print(f)`), which indents
// a block body one step (4 columns) inside `function`/`if`/`for`/`while`/`repeat`/`atomic` and puts
// `elif`/`else`/`fi`/`od`/`until`/`end` back at the level of the line that opened the block. This formatter
// applies that indentation to the file as written and changes nothing else: every token, comment and line
// break stays where it is, so the result reads exactly as before (`mep-gap-format-test --check` compares
// the token streams). A line inside an open bracket, or continuing a statement from the line above, is one
// step deeper than the line it continues. Lines that start inside a multi-line string are left alone.
#include "gap_format.h"

#include <algorithm>
#include <vector>

#include "gap_parse.h"

namespace gapfmt {

namespace {

using gapparse::T;

struct Frame {
    char kind = 'B';  // 'F' function, 'I' if, 'L' for/while/atomic, 'R' repeat, 'B' bracket, '0' the file
    int base = 0;     // the indent of the line that opened it
    bool header = false;  // before the body: a function's parameters, an if's condition, a loop's head
    bool in_stmt = false;  // a statement of this block has started and not yet ended with `;`
};

bool IsCloser(T k) {
    return k == T::End || k == T::Fi || k == T::Od || k == T::Until || k == T::Elif || k == T::Else ||
           k == T::RParen || k == T::RBracket || k == T::RBrace;
}

// The first token kind of the next line after @p li that starts with code.
T NextCodeLine(const std::vector<T> &first_kind, size_t li, const std::vector<std::string_view> &lines) {
    for (size_t j = li + 1; j < first_kind.size(); j++) {
        if (first_kind[j] != T::Eof) return first_kind[j];
        const std::string_view l = lines[j];
        const size_t a = l.find_first_not_of(" \t\r\f");
        if (a != std::string_view::npos && l[a] != '#') return T::Eof;  // a line starting inside a token
    }
    return T::Eof;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    const gapparse::Parse p = gapparse::ParseGap(src);
    if (!p.errors.empty()) {
        r.error = "syntax error: " + p.errors[0].message;
        r.error_line = p.errors[0].line + 1;
        return r;
    }
    const int unit = std::max(1, std::min(opts.indent, 16));

    // The source's lines.
    std::vector<std::string_view> lines;
    std::vector<uint32_t> starts;
    for (size_t i = 0;;) {
        starts.push_back(static_cast<uint32_t>(i));
        const size_t j = src.find('\n', i);
        if (j == std::string_view::npos) {
            lines.push_back(src.substr(i));
            break;
        }
        lines.push_back(src.substr(i, j - i));
        i = j + 1;
    }
    auto line_of = [&](uint32_t off) {
        return static_cast<int>(std::upper_bound(starts.begin(), starts.end(), off) - starts.begin()) - 1;
    };
    // Lines that start inside a token (verbatim) and lines whose end is inside one (keep trailing blanks).
    std::vector<char> verbatim(lines.size(), 0), keep_end(lines.size(), 0);
    for (const gapparse::Token &t : p.tokens) {
        if (t.kind == T::Eof || t.end <= t.begin) continue;
        const int a = line_of(t.begin), b = line_of(t.end - 1);
        for (int l = a; l < b; l++) keep_end[static_cast<size_t>(l)] = 1;
        for (int l = a + 1; l <= b; l++) verbatim[static_cast<size_t>(l)] = 1;
    }

    // The kind of the token each line starts with (Eof: a blank or comment-only line).
    std::vector<T> first_kind(lines.size(), T::Eof);
    for (const gapparse::Token &t : p.tokens) {
        if (t.kind == T::Eof) continue;
        const size_t l = static_cast<size_t>(line_of(t.begin));
        if (first_kind[l] != T::Eof || verbatim[l]) continue;
        size_t a = 0;
        while (a < lines[l].size() && (lines[l][a] == ' ' || lines[l][a] == '\t' || lines[l][a] == '\f')) a++;
        if (t.begin == starts[l] + a) first_kind[l] = t.kind;
    }

    std::vector<Frame> st(1);
    st[0].kind = '0';
    st[0].base = -unit;
    auto inner = [&](const Frame &f) { return f.base + unit; };
    auto stmt = [&]() {
        if (!st.back().header) st.back().in_stmt = true;
    };
    auto pop_to = [&](char kind) {
        while (st.size() > 1 && st.back().kind != kind) st.pop_back();
        if (st.size() > 1) st.pop_back();
    };
    auto find = [&](char kind) -> const Frame & {
        for (size_t i = st.size(); i-- > 1;)
            if (st[i].kind == kind) return st[i];
        return st[0];
    };

    std::string out;
    size_t ti = 0;
    int last_indent = 0;
    for (size_t li = 0; li < lines.size(); li++) {
        std::string_view line = lines[li];
        while (ti < p.tokens.size() && p.tokens[ti].kind != T::Eof && line_of(p.tokens[ti].begin) < static_cast<int>(li))
            ti++;
        int indent = last_indent;
        if (verbatim[li]) {
            out += line;
        } else {
            size_t a = 0;
            while (a < line.size() && (line[a] == ' ' || line[a] == '\t' || line[a] == '\r' || line[a] == '\f')) a++;
            std::string_view body = line.substr(a);
            if (!keep_end[li])
                while (!body.empty() && (body.back() == ' ' || body.back() == '\t' || body.back() == '\r'))
                    body.remove_suffix(1);
            if (!body.empty()) {
                const bool tok_first = ti < p.tokens.size() && p.tokens[ti].kind != T::Eof &&
                                       line_of(p.tokens[ti].begin) == static_cast<int>(li) &&
                                       p.tokens[ti].begin == starts[li] + a;
                const T k = tok_first ? p.tokens[ti].kind : T::Eof;
                if (tok_first && IsCloser(k)) {
                    char kind = 'B';
                    if (k == T::End) kind = 'F';
                    if (k == T::Fi || k == T::Elif || k == T::Else) kind = 'I';
                    if (k == T::Od) kind = 'L';
                    if (k == T::Until) kind = 'R';
                    indent = std::max(0, find(kind).base);
                } else if (!tok_first && NextCodeLine(first_kind, li, lines) != T::Eof &&
                           (NextCodeLine(first_kind, li, lines) == T::Elif ||
                            NextCodeLine(first_kind, li, lines) == T::Else)) {
                    // A comment introducing an elif/else branch sits at the branch's level.
                    indent = std::max(0, find('I').base);
                } else {
                    const Frame &top = st.back();
                    indent = inner(top) + (top.kind != 'B' && top.in_stmt && !top.header ? unit : 0);
                    indent = std::max(0, indent);
                }
                out.append(static_cast<size_t>(indent), ' ');
                out += body;
                last_indent = indent;
            }
        }
        // The tokens that start on this line update the blocks.
        for (; ti < p.tokens.size() && p.tokens[ti].kind != T::Eof && line_of(p.tokens[ti].begin) == static_cast<int>(li);
             ti++) {
            const T k = p.tokens[ti].kind;
            Frame f;
            f.base = indent;
            switch (k) {
            case T::Function:
                stmt();
                f.kind = 'F';
                f.header = true;
                st.push_back(f);
                break;
            case T::If:
                stmt();
                f.kind = 'I';
                f.header = true;
                st.push_back(f);
                break;
            case T::For:
            case T::While:
                stmt();
                f.kind = 'L';
                f.header = true;
                st.push_back(f);
                break;
            case T::Atomic:
                if (ti + 1 < p.tokens.size() && p.tokens[ti + 1].kind == T::Function) break;
                stmt();
                f.kind = 'L';
                f.header = true;
                st.push_back(f);
                break;
            case T::Repeat:
                stmt();
                f.kind = 'R';
                st.push_back(f);
                break;
            case T::Then:
            case T::Do:
            case T::Else:
                st.back().header = false;
                st.back().in_stmt = false;
                break;
            case T::Elif:
                st.back().header = true;
                st.back().in_stmt = false;
                break;
            case T::Fi:
                pop_to('I');
                break;
            case T::Od:
                pop_to('L');
                break;
            case T::End:
                pop_to('F');
                break;
            case T::Until:
                pop_to('R');
                stmt();
                break;
            case T::LParen:
            case T::LBracket:
            case T::LBrace:
            case T::BangBracket:
                stmt();
                f.kind = 'B';
                st.push_back(f);
                break;
            case T::RParen:
            case T::RBracket:
            case T::RBrace:
                pop_to('B');
                if (k == T::RParen && st.back().kind == 'F' && st.back().header) {
                    st.back().header = false;  // the parameter list: the body starts
                    st.back().in_stmt = false;
                }
                break;
            case T::Semi:
            case T::DualSemi:
                st.back().in_stmt = false;
                break;
            default:
                stmt();
                break;
            }
        }
        if (li + 1 < lines.size()) out += '\n';
    }
    // No trailing blank lines; one final newline.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    if (!out.empty()) out += '\n';
    r.ok = true;
    r.text = out;
    return r;
}

}  // namespace gapfmt
