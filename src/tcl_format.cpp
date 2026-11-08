// mep's Tcl formatter: see tcl_format.h.
//
// A port of tclfmt from tclint 0.6.2 (format.py, parser.py, lexer.py,
// syntax_tree.py and commands/builtin.py; MIT License, Copyright (c) Noah
// Moroze). The lexer, parser and command table live in
// tcl_format_parse.cpp; this file is the printer (format.py) plus the
// safety net around it.
//
// tclfmt does not wrap lines: it reindents scripts that builtin commands
// take as arguments (proc/if/foreach/... bodies, expr expressions, proc
// argument lists, switch arms), normalises the whitespace between words
// and commands, keeps backslash-newline continuations as `\` + one indent,
// and caps runs of blank lines. Words it cannot see into (data in braces,
// quoted strings) are copied verbatim.
//
// Self-check: the result is parsed again and must give the same syntax
// tree as the input (tclfmt's own `--debug` check, plus the VarSub details
// tclint's tree leaves out); otherwise the input is returned unchanged
// with ok=false.
#include "tcl_format.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tcl_format_internal.h"

namespace tclfmt {

namespace {

using detail::K;
using detail::Node;
using detail::Pos;
using Lines = std::vector<std::string>;

class Printer {
  public:
    Printer(const Options &o, const std::string &src) : opts_(o) {
        indent_ = o.tabs ? std::string("\t") : std::string(static_cast<size_t>(std::max(0, o.indent)), ' ');
        size_t start = 0;
        for (;;) {
            const size_t nl = src.find('\n', start);
            if (nl == std::string::npos) {
                script_.push_back(src.substr(start));
                break;
            }
            script_.push_back(src.substr(start, nl - start));
            start = nl + 1;
        }
    }

    std::string Top(Node *tree) {
        const Lines lines = ScriptContents(tree);
        std::string out;
        for (size_t k = 0; k < lines.size(); k++) {
            if (k) out += '\n';
            out += lines[k];
        }
        out += '\n';
        return out;
    }

  private:
    const Options &opts_;
    std::string indent_;
    Lines script_;
    std::vector<std::unique_ptr<Node>> literal_blocks_;

    static void Append(Lines &out, const Lines &lines, size_t from = 0) {
        for (size_t k = from; k < lines.size(); k++) out.push_back(lines[k]);
    }

    // `out[-1] += lines[0]; out.extend(lines[1:])`
    static void Join(Lines &out, const Lines &lines) {
        if (lines.empty()) return;
        out.back() += lines[0];
        Append(out, lines, 1);
    }

    Lines Indent(const Lines &lines, size_t from = 0) const {
        Lines r;
        for (size_t k = from; k < lines.size(); k++) r.push_back(lines[k].empty() ? std::string() : indent_ + lines[k]);
        return r;
    }

    Lines Brace(Lines lines) const {
        const std::string sp = opts_.spaces_in_braces ? " " : "";
        if (lines.size() == 1 && lines[0].empty()) return {"{" + sp + "}"};
        if (lines.empty()) return {"{" + sp + "}"};
        lines[0] = "{" + sp + lines[0];
        lines.back() += sp + "}";
        return lines;
    }

    static std::string Strip(const std::string &s) {
        auto space = [](char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F); };
        size_t a = 0, b = s.size();
        while (a < b && space(s[a])) a++;
        while (b > a && space(s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    Node *LiteralBlock(int from_line, std::optional<int> to_line, Pos end) {
        auto n = std::make_unique<Node>();
        n->kind = K::LiteralBlock;
        const size_t a = std::min(static_cast<size_t>(std::max(0, from_line)), script_.size());
        size_t b = to_line ? static_cast<size_t>(std::max(0, *to_line)) : script_.size();
        b = std::min(b, script_.size());
        for (size_t k = a; k < b; k++) n->block.push_back(script_[k]);
        n->pos = {from_line + 1, 1};
        n->end = end;
        literal_blocks_.push_back(std::move(n));
        return literal_blocks_.back().get();
    }

    Lines ScriptContents(Node *script) {
        std::vector<Node *> to_format;
        std::optional<int> skip;
        for (Node *child : script->kids) {
            if (!skip) to_format.push_back(child);
            if (child->kind != K::Comment) continue;
            const std::string v = Strip(child->value);
            if (v == "tclfmt-disable") {
                if (!skip) skip = child->pos.line;
            } else if (v == "tclfmt-enable" && skip) {
                const int end = child->pos.line;
                to_format.push_back(LiteralBlock(*skip, end, Pos{end, 1}));
                skip.reset();
            }
        }
        if (skip) to_format.push_back(LiteralBlock(*skip, std::nullopt, script->end));

        Lines formatted{""};
        bool have_last = false;
        bool swallowed = false;
        int last_line = 0;
        for (Node *child : to_format) {
            if (have_last) {
                if (last_line == child->pos.line) {
                    formatted.back() += child->kind == K::Comment ? " ;" : "; ";
                } else {
                    int newlines = std::min(child->pos.line - last_line, opts_.max_blank_lines + 1);
                    // A comment ending in a backslash continued onto a blank
                    // line: keep that blank line, or the backslash would pull
                    // the next command into the comment (tclfmt drops it).
                    if (swallowed) newlines = std::max(newlines, 2);
                    for (int k = 0; k < newlines; k++) formatted.emplace_back();
                }
            }
            have_last = true;
            last_line = child->end.line;
            swallowed = false;
            if (child->kind == K::Comment) {
                const int text_end =
                    child->pos.line + static_cast<int>(std::count(child->value.begin(), child->value.end(), '\n'));
                if (text_end < last_line) {
                    last_line = text_end;
                    swallowed = true;
                }
            }
            Join(formatted, Format(child));
        }
        return formatted;
    }

    Lines Format(Node *n) {
        switch (n->kind) {
        case K::Script: return FormatScript(n, true);
        case K::Command: return FormatCommand(n);
        case K::Comment: return {"#" + n->value};
        case K::CommandSub: return FormatCommandSub(n);
        case K::BareWord: return {n->value};
        case K::QuotedWord: return FormatQuoted(n);
        case K::BracedWord: return {"{" + n->value + "}"};
        case K::Compound: {
            Lines out{""};
            for (Node *c : n->kids) Join(out, Format(c));
            return out;
        }
        case K::VarSub: return FormatVarSub(n);
        case K::ArgExpansion: {
            Lines lines = Format(n->kids[0]);
            lines[0] = "{*}" + lines[0];
            return lines;
        }
        case K::List: return FormatList(n);
        case K::Expression: return FormatExpression(n);
        case K::BracedExpression: {
            Lines out{""};
            for (Node *c : n->kids) Join(out, Format(c));
            if (n->pos.line == n->end.line) return Brace(out);
            Lines r{"{"};
            Append(r, Indent(out));
            r.emplace_back("}");
            return r;
        }
        case K::ParenExpression: return FormatParen(n);
        case K::UnaryOp: {
            const Lines op = Format(n->kids[0]);
            Lines lines = Format(n->kids[1]);
            lines[0] = op[0] + lines[0];
            return lines;
        }
        case K::BinaryOp:
        case K::TernaryOp: return FormatOp(n);
        case K::Function: return FormatFunction(n);
        case K::LiteralBlock: return n->block.empty() ? Lines{""} : n->block;
        }
        return {""};
    }

    Lines FormatScript(Node *script, bool should_indent) {
        Lines lines = ScriptContents(script);
        if (script->pos.line == script->end.line) return Brace(lines);
        std::string open = "{";
        if (!script->kids.empty() && script->kids[0]->kind == K::Comment &&
            script->pos.line == script->kids[0]->pos.line) {
            open += " " + lines[0];
            lines.erase(lines.begin());
        }
        Lines r{open};
        Append(r, should_indent ? Indent(lines) : lines);
        r.emplace_back("}");
        return r;
    }

    Lines FormatCommand(Node *cmd) {
        Node *routine = cmd->kids[0];
        const std::string *rc = detail::Contents(routine);
        const std::string *a0 = cmd->kids.size() > 1 ? detail::Contents(cmd->kids[1]) : nullptr;
        const bool ns_eval = rc && *rc == "namespace" && a0 && *a0 == "eval";
        const bool should_indent = !ns_eval || opts_.indent_namespace_eval;
        bool hanging = false;
        Lines out = Format(routine);
        int last_line = routine->end.line;
        for (size_t k = 1; k < cmd->kids.size(); k++) {
            Node *child = cmd->kids[k];
            const Lines lines = child->kind == K::Script ? FormatScript(child, should_indent) : Format(child);
            if (last_line == child->pos.line) {
                out.back() += " " + lines[0];
            } else {
                out.back() += " \\";
                out.push_back(indent_ + lines[0]);
                hanging = true;
            }
            Append(out, hanging ? Indent(lines, 1) : Lines(lines.begin() + 1, lines.end()));
            last_line = child->end.line;
        }
        return out;
    }

    Lines FormatCommandSub(Node *sub) {
        if (sub->kids.empty()) return {"[]"};
        const Lines contents = ScriptContents(sub);
        Lines out;
        if (sub->kids.size() > 1 && contents.size() > 1) {
            out.emplace_back("[");
            Append(out, Indent(contents));
            out.emplace_back("]");
        } else {
            out.push_back("[" + contents[0]);
            Append(out, contents, 1);
            out.back() += "]";
        }
        return out;
    }

    Lines FormatQuoted(Node *w) {
        if (const std::string *c = detail::Contents(w)) return {"\"" + *c + "\""};
        std::string s;
        for (Node *child : w->kids) {
            const Lines lines = Format(child);
            for (size_t k = 0; k < lines.size(); k++) {
                if (k) s += '\n';
                s += lines[k];
            }
        }
        return {"\"" + s + "\""};
    }

    Lines FormatVarSub(Node *v) {
        Lines out{v->braced ? "${" + v->value + "}" : "$" + v->value};
        // tclfmt drops an empty index (`$a()` -> `$a`), which names another
        // variable; keep the parentheses whenever the source had them.
        if (!v->kids.empty() || v->paren) {
            out.back() += "(";
            for (Node *c : v->kids) Join(out, Format(c));
            out.back() += ")";
        }
        return out;
    }

    Lines FormatList(Node *list) {
        Lines contents{""};
        bool have_last = false;
        int last_line = 0;
        for (Node *child : list->kids) {
            if (have_last) {
                if (last_line == child->pos.line) {
                    contents.back() += " ";
                } else {
                    const int newlines = std::min(child->pos.line - last_line, 3);
                    for (int k = 0; k < newlines; k++) contents.emplace_back();
                }
            }
            Join(contents, Format(child));
            have_last = true;
            last_line = child->end.line;
        }
        if (list->pos.line == list->end.line) return Brace(contents);
        Lines r{"{"};
        Append(r, Indent(contents));
        r.emplace_back("}");
        return r;
    }

    Lines FormatExpression(Node *e) {
        Lines out{""};
        for (Node *c : e->kids) {
            const Lines lines = Format(c);
            out.back() += lines[0];
            for (size_t k = 1; k < lines.size(); k++) {
                out.back() += " \\";
                Append(out, Indent({lines[k]}));
            }
        }
        // Quotes around the expression: its start differs from its first child's.
        if (!e->kids.empty() && e->pos.col != e->kids[0]->pos.col) {
            out[0] = "\"" + out[0];
            out.back() += "\"";
        }
        return out;
    }

    Lines FormatParen(Node *e) {
        Node *body = e->kids[0];
        Lines out{"("};
        const Lines lines = Format(body);
        if (e->pos.line != body->pos.line) {
            Append(out, lines);
        } else {
            Join(out, lines);
        }
        Lines r{out[0]};
        Append(r, Indent(out, 1));
        if (e->end.line != body->end.line) {
            r.emplace_back(")");
        } else {
            r.back() += ")";
        }
        return r;
    }

    Lines FormatOp(Node *e) {
        Lines out = Format(e->kids[0]);
        Node *last = e->kids[0];
        for (size_t k = 1; k < e->kids.size(); k++) {
            Node *next = e->kids[k];
            const Lines lines = Format(next);
            if (last->end.line != next->pos.line) {
                Append(out, lines);
            } else {
                out.back() += " " + lines[0];
                Append(out, lines, 1);
            }
            last = next;
        }
        return out;
    }

    Lines FormatFunction(Node *f) {
        const Lines name = Format(f->kids[0]);
        Lines out{name[0] + "("};
        Node *last = f->kids[0];
        for (size_t k = 1; k < f->kids.size(); k++) {
            Node *child = f->kids[k];
            if (k > 1) out.back() += ",";
            const Lines lines = Format(child);
            if (last->end.line != child->pos.line) {
                Append(out, lines);
            } else {
                if (k > 1) out.back() += " ";
                Join(out, lines);
            }
            last = child;
        }
        Lines r{out[0]};
        Append(r, Indent(out, 1));
        if (last->end.line != f->end.line) {
            r.emplace_back(")");
        } else {
            r.back() += ")";
        }
        return r;
    }
};

bool SameTree(const Node *a, const Node *b) {
    if (a->kind != b->kind || a->value != b->value || a->braced != b->braced || a->paren != b->paren ||
        a->kids.size() != b->kids.size())
        return false;
    for (size_t k = 0; k < a->kids.size(); k++)
        if (!SameTree(a->kids[k], b->kids[k])) return false;
    return true;
}

bool ValidUtf8(const std::string &s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        unsigned int cp = 0;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

// Python's universal newlines: \r\n and a lone \r both read as \n.
std::string Normalize(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\r') {
            out += '\n';
            if (i + 1 < src.size() && src[i + 1] == '\n') i++;
        } else {
            out += src[i];
        }
    }
    return out;
}

Result Refuse(std::string_view src, std::string why, int line) {
    Result r;
    r.text = std::string(src);
    r.error = std::move(why);
    r.error_line = line;
    return r;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    const std::string text = Normalize(src);
    if (!ValidUtf8(text)) return Refuse(src, "not valid UTF-8", 0);
    std::string out;
    detail::Parser parser;
    Node *tree = nullptr;
    try {
        tree = parser.Parse(text);
    } catch (const detail::SyntaxError &e) {
        return Refuse(src, "syntax error: " + e.message, e.pos.line);
    } catch (const detail::DepthError &) {
        return Refuse(src, "nested too deeply to format", 0);
    }
    out = Printer(opts, text).Top(tree);

    detail::Parser again;
    try {
        Node *check = again.Parse(out);
        if (!SameTree(tree, check)) return Refuse(src, "formatter bug: the result parses differently", 0);
    } catch (const detail::SyntaxError &e) {
        return Refuse(src, "formatter bug: the result does not parse (" + e.message + ")", 0);
    } catch (const detail::DepthError &) {
        return Refuse(src, "nested too deeply to format", 0);
    }
    Result r;
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace tclfmt
